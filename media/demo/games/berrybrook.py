"""Berrybrook — a cozy hillside berry farm at golden hour (original IP; benchmark: the best cozy farm sims).

A little farm on a south-facing hill: rows of strawberries in straw, a glasshouse of seedlings, a
cottage with a deep porch and warm windows, a lily pond, a smock windmill turning on the ridge, a
roadside berry stand, chickens, a sleepy dog and butterflies over the flower beds. The whole kit is
modelled procedurally in Blender by the crew (berrybrook_blender.py); ground textures are Poly Haven
(CC0) and procedural. Gameplay: click a soil patch to plant, watch it grow, harvest for coins; a cozy
HUD (day, time, coins, a hotbar); a short chat with the neighbour, Juniper.
"""
import hashlib
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import polyhaven as ph  # noqa: E402
from kit import Studio, rnd  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
PROJECT = os.path.join(ROOT, "examples", "berrybrook")

META = dict(
    id="berrybrook", title="Berrybrook", genre="Cozy Farming Life Sim", mood="golden hour, warm and gentle",
    pitch="Grandma's berry farm is yours now. Plant a row, mind the hens, and sell the sweetest strawberries on the hill.",
    view=dict(eye=[24, 16, 40], target=[0, 4, -6]),
    assets="Procedural (Blender via Skywalker DCC), Poly Haven (CC0) ground textures",
)


# =============================================================================================
# The kit (Blender) and its materials
# =============================================================================================
def model_kit(pix, only=None):
    """Pixel models the farm kit in Blender (cached on the script hash), then imports every part."""
    src = open(os.path.join(HERE, "berrybrook_blender.py")).read()
    args = json.dumps({"seed": 11, **({"only": only} if only else {})})
    digest = hashlib.sha1((src + args).encode()).hexdigest()
    out = os.path.join(PROJECT, "dcc", "kit")
    stamp = os.path.join(out, ".build")
    if not (os.path.exists(stamp) and open(stamp).read() == digest):
        r = pix.call("dcc_run_script", script=src, name="kit", out_dir="dcc/kit", args=[args], timeout_s=1800,
                     description="Berrybrook farm kit")
        assets = r.get("result", {}).get("assets", {})
        old = {}
        if only and os.path.exists(os.path.join(out, "assets.json")):
            old = json.load(open(os.path.join(out, "assets.json")))
        old.update(assets)
        with open(os.path.join(out, "assets.json"), "w") as f:
            json.dump(old, f, indent=1)
        with open(stamp, "w") as f:
            f.write(digest)
    assets = json.load(open(os.path.join(out, "assets.json")))
    meshes = {}
    for key, parts in assets.items():
        for part, info in parts.items():
            res = pix.call("asset_import", path=f"dcc/kit/{info['file']}", normalize=False)
            meshes.setdefault(key, {})[part] = res["mesh"]
    return meshes


def kit_materials(pix):
    """One engine material per kit part; vertex colors carry the paint, these carry the surface."""
    import berrybrook_art as art
    art.berry_seeds(os.path.join(PROJECT, "textures", "berry_seeds.png"))
    wood = pix.texture("wood", "wood_grain", color1="#f2ece4", color2="#d9cfc2", scale=6, bump=0.6, tiling=0.9)
    planks = pix.texture("planks", "planks_soft", color1="#f4eee6", color2="#ddd2c4", color3="#b9ab98", scale=4, bump=0.7, tiling=0.45)
    plaster = pix.texture("noise", "plaster_soft", color1="#ffffff", color2="#ece6dc", scale=10, variation=0.35, bump=0.35, tiling=0.8)
    stone = pix.texture("rock", "stone_soft", color1="#ffffff", color2="#ebe7e1", color3="#d8d2ca", scale=4, variation=0.25, bump=0.8, tiling=0.8)
    dirt = pix.texture("dirt", "soil_soft", color1="#ffffff", color2="#cfc6bb", scale=8, variation=0.5, bump=1.0, tiling=1.2)
    M = {"paint": wood, "deck": planks, "stone": stone, "soil": dirt}
    for name, fields in (
            ("roof", dict(color="#ffffff", roughness=0.72)),
            ("plaster", dict(color="#ffffff", roughness=0.92)),
            ("glow", dict(color="#ffffff", emissive=[1.0, 0.72, 0.42, 2.6], roughness=0.5)),
            ("bulb", dict(color="#ffffff", emissive=[1.0, 0.78, 0.48, 7.0], roughness=0.4)),
            ("glass", dict(color="#e4f2f660", roughness=0.04, clearcoat=1.0, doubleSided=True, metallic=0.0)),
            ("leaf", dict(color="#ffffff", roughness=0.72, subsurface=0.45, doubleSided=True)),
            ("stem", dict(color="#ffffff", roughness=0.6, subsurface=0.4, doubleSided=True)),
            ("calyx", dict(color="#ffffff", roughness=0.55, subsurface=0.5, doubleSided=True)),
            ("berry", dict(color="#ffffff", roughness=0.22, clearcoat=0.7, subsurface=0.35, texture="textures/berry_seeds.png",
                           triplanar=True, tilingU=14, tilingV=14)),
            ("berry_unripe", dict(color="#ffffff", roughness=0.55, subsurface=0.4, texture="textures/berry_seeds.png", triplanar=True,
                                  tilingU=14, tilingV=14)),
            ("flower", dict(color="#ffffff", roughness=0.6, subsurface=0.7, doubleSided=True)),
            ("straw", dict(color="#ffffff", roughness=0.85, subsurface=0.3, doubleSided=True)),
            ("canopy", dict(color="#ffffff", roughness=0.8, subsurface=0.45)),
            ("blossom", dict(color="#ffffff", roughness=0.7, subsurface=0.6)),
            ("fruit", dict(color="#ffffff", roughness=0.3, clearcoat=0.5)),
            ("bark", dict(color="#ffffff", roughness=0.9)),
            ("leafy", dict(color="#ffffff", roughness=0.7, subsurface=0.5, doubleSided=True)),
            ("petals", dict(color="#ffffff", roughness=0.65, subsurface=0.6, doubleSided=True)),
            ("metal", dict(color="#ffffff", metallic=0.7, roughness=0.42)),
            ("metal_paint", dict(color="#ffffff", metallic=0.2, roughness=0.38, clearcoat=0.3)),
            ("cloth", dict(color="#ffffff", roughness=0.9, subsurface=0.3, doubleSided=True)),
            ("canvas", dict(color="#ffffff", roughness=0.85, subsurface=0.45, doubleSided=True)),
            ("hay", dict(color="#ffffff", roughness=0.9, subsurface=0.25, doubleSided=True)),
            ("pad", dict(color="#ffffff", roughness=0.3, subsurface=0.3, doubleSided=True)),
            ("lily", dict(color="#ffffff", roughness=0.55, subsurface=0.7, doubleSided=True)),
            ("cattail", dict(color="#ffffff", roughness=0.85)),
            ("feather", dict(color="#ffffff", roughness=0.85, subsurface=0.25)),
            ("fur", dict(color="#ffffff", roughness=0.9, subsurface=0.2)),
            ("comb", dict(color="#ffffff", roughness=0.5, subsurface=0.4)),
            ("beak", dict(color="#ffffff", roughness=0.5)),
            ("eye", dict(color="#ffffff", roughness=0.15, clearcoat=1.0)),
            ("nose", dict(color="#ffffff", roughness=0.25, clearcoat=0.6)),
            ("wing", dict(color="#ffffff", roughness=0.6, subsurface=0.7, doubleSided=True)),
            ("body", dict(color="#ffffff", roughness=0.6)),
            ("chalk", dict(color="#ffffff", roughness=0.95)),
            ("jar", dict(color="#ffffff", roughness=0.12, clearcoat=1.0, subsurface=0.4)),
            ("pot", dict(color="#ffffff", roughness=0.85)),
            ("basket", dict(color="#ffffff", roughness=0.8))):
        path = f"materials/kit_{name}.mat.json"
        pix.material(path, **fields)
        M[name] = path
    # parts that share a textured surface
    M["boards"] = planks
    M["hub"] = wood
    M["sail"] = wood
    return M


def place_kit(b, meshes, M, key, name, pos, yaw=0.0, parent=None, tags=None, scale=1.0, rot=None, color=None, shadows=True):
    b.e(name, pos=pos, rot=rot or (0, yaw, 0), parent=parent, tags=tags, scale=scale)
    for part, mesh in meshes[key].items():
        extra = {}
        if part in ("glow", "bulb", "glass") or not shadows:
            extra["castShadows"] = False
        if color is not None and part in ("wing",):
            extra["color"] = color
        b.e(f"{name} {part}", mesh, parent=name, material=M[part], **extra)
    return name


def lineup(studio):
    """Look-dev: every kit asset in a row on a flat ground."""
    nim, pix = studio.agent("Nimbus"), studio.agent("Pixel")
    nim.call("scene_new", name="Kit lineup", empty=True)
    meshes = model_kit(pix)
    M = kit_materials(pix)
    nim.e("Ground", "cube", pos=(0, -0.5, 0), scale=(400, 1, 400), color="#7da35a", roughness=0.9)
    x = 0.0
    for key in meshes:
        place_kit(pix, meshes, M, key, f"K {key}", (x, 0, 0))
        x += 12 if key in ("farmhouse", "greenhouse", "windmill", "windmill_sails", "strawberry_row", "strawberry_row_b") else 4
    pix.flush("Kit lineup")
    nim.flush("Ground")
    nim.call("environment_update", **MOODS["golden"])
    nim.call("scene_save", path="scenes/lineup.sky.json")


# =============================================================================================
# Layout (world meters; x east, z south, y up). The farm sits on a gentle south-facing slope;
# the windmill stands on a knoll to the north-east; the lane runs east-west along the south.
# =============================================================================================
TER_SIZE, TER_RES, HMIN, HMAX = 320.0, 513, -40.0, 40.0
HOUSE = (-14.0, -17.0)           # farmhouse center (front faces +z)
GREENHOUSE = (-31.0, -13.0)
FIELD = (13.0, -17.0)            # berry field center: rows run east-west
ROWS = 10
ROW_GAP = 1.4
STALL = (15.0, 21.6)
MILL = (37.0, -52.0)
POND = (-25.0, 14.0, 8.5)        # x, z, radius
PLOTS = [(-3.6 + 1.55 * (k % 3), -7.2 + 1.55 * (k // 3)) for k in range(6)]   # gameplay soil patches
LANE_Z = 25.0


def _ss(e0, e1, x):
    t = max(0.0, min(1.0, (x - e0) / (e1 - e0)))
    return t * t * (3 - 2 * t)


def plane(x, z):
    """The farm's gentle slope: rises to the north (negative z)."""
    return -0.06 * z + 0.25 * math.sin(x * 0.11 + 0.7) * math.sin(z * 0.09 + 1.1)


# flat pads: (x, z, radius, follow_slope) — buildings sit level, the field keeps the gentle slope
PADS = [(HOUSE[0], HOUSE[1] + 1.2, 9.5, False), (GREENHOUSE[0], GREENHOUSE[1], 6.0, False), (STALL[0], STALL[1], 4.0, False),
        (FIELD[0] - 5, FIELD[1], 9.5, True), (FIELD[0] + 5, FIELD[1], 9.5, True), (-2.0, -5.5, 6.5, False)]


def pond_level():
    return round(-0.06 * POND[1] - 0.3, 3)


def _raw(x, z):
    h = plane(x, z)
    # rolling hills beyond the farm, rising into a backdrop to the north
    d = math.hypot((x - 4) / 1.25, z + 6)
    far = _ss(52, 115, d)
    hills = (6.5 * math.sin(x * 0.019 + 1.3) * math.cos(z * 0.016 - 0.4) + 3.2 * math.sin(x * 0.043 + z * 0.029) +
             1.4 * math.sin(x * 0.087 - z * 0.069 + 2.0) + 7.0 * _ss(-60, -150, z) + 4.0 * _ss(80, 150, abs(x)))
    h += hills * far
    # the windmill knoll
    h += 5.5 * math.exp(-((x - MILL[0]) ** 2 + (z - MILL[1]) ** 2) / (2 * 15.0 ** 2))
    return h


H_MILL_TOP = _raw(*MILL)


def H(x, z):
    """Analytic height of the farm terrain (the heightmap is sampled from it)."""
    h = _raw(x, z)
    for px, pz, r, follow in PADS:
        w = 1.0 - _ss(r, r + 6.0, math.hypot(x - px, z - pz))
        if w > 0:
            target = -0.06 * z if follow else -0.06 * pz
            h += (target - h) * w
    mx, mz = MILL
    w = 1.0 - _ss(5.0, 10.0, math.hypot(x - mx, z - mz))
    if w > 0:
        h += (H_MILL_TOP - h) * w
    # the pond: a soft bowl below the water line, with a gentle bank
    px, pz, pr = POND
    dp = math.hypot((x - px) / 1.15, z - pz)
    wl = pond_level()
    if dp < pr + 5.0:
        bowl = wl - 1.3 * max(0.0, 1.0 - (dp / pr) ** 2) - 0.15
        if dp < pr - 0.5:
            h = bowl
        else:
            bank = _ss(pr - 0.5, pr + 5.0, dp)
            h = bowl * (1 - bank) + h * bank
    # the lane: a shallow cut along the south
    h -= 0.12 * (1.0 - _ss(1.5, 3.5, abs(z - lane_z(x))))
    # sink the edges under the far hills
    h -= 22.0 * _ss(125, 158, max(abs(x), abs(z)))
    return h


def lane_z(x):
    return LANE_Z + 2.0 * math.sin(x * 0.03)


def write_heightmap(path):
    from PIL import Image
    n = TER_RES
    data = []
    for j in range(n):
        z = -TER_SIZE / 2 + TER_SIZE * j / (n - 1)
        for i in range(n):
            x = -TER_SIZE / 2 + TER_SIZE * i / (n - 1)
            v = (H(x, z) - HMIN) / (HMAX - HMIN)
            data.append(int(max(0, min(65535, round(v * 65535)))))
    im = Image.new("I;16", (n, n))
    im.putdata(data)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.save(path)


# =============================================================================================
# Build
# =============================================================================================
def ground_layers(pix):
    grass = ph.texture(pix, "leafy_grass", tiling=1.0, triplanar=False)
    lane = pix.texture("dirt", "lane_dirt", color1="#c9a27a", color2="#a98462", color3="#8a6a4c", scale=10, variation=0.4, bump=0.8,
                       tiling=0.5)
    tilled = pix.texture("noise", "tilled_soil", color1="#7a5338", color2="#5f412c", scale=9, variation=0.35, bump=0.5, tiling=0.6)
    mud = ph.texture(pix, "brown_mud_leaves_01", tiling=1.0, triplanar=False)
    return [
        {"name": "grass", "material": grass, "tiling": 3.0, "color": "#c6e296"},
        {"name": "meadow", "material": grass, "tiling": 2.4, "color": "#a9cc7a", "heightMin": 2.5, "noise": 0.7, "sharpness": 0.35},
        {"name": "lane", "material": lane, "tiling": 1.6, "heightMin": 9000},
        {"name": "tilled", "material": tilled, "tiling": 1.4, "heightMin": 9000},
        {"name": "bank", "material": mud, "tiling": 2.5, "color": "#c9b79a", "heightMax": pond_level() + 0.35, "sharpness": 0.6},
        {"name": "lawn", "material": grass, "tiling": 3.0, "color": "#c4df92", "heightMin": 9000},
        {"name": "bloom", "material": grass, "tiling": 3.0, "color": "#c2d98c", "heightMin": 9000},
        {"name": "flowerbed", "material": tilled, "tiling": 1.4, "color": "#d8c2b0", "heightMin": 9000},
    ]


GRASS, MEADOW, LANE, TILLED, BANK, LAWN, BLOOM, BED = 0, 1, 2, 3, 4, 5, 6, 7


def path_points(pts, step=1.0):
    out = []
    for (x0, z0), (x1, z1) in zip(pts, pts[1:]):
        n = max(1, int(math.dist((x0, z0), (x1, z1)) / step))
        for k in range(n):
            t = k / n
            out.append((x0 + (x1 - x0) * t, z0 + (z1 - z0) * t))
    out.append(pts[-1])
    return out


# The farm paths: the drive up from the lane to the house, the path to the field and the greenhouse.
DRIVE = [(4.0, 24.0), (3.0, 14.0), (0.0, 4.0), (-6.0, -4.0), (-11.0, -8.5)]
FIELD_PATH = [(-2.0, -5.0), (3.0, -9.0), (4.5, -17.0), (5.0, -26.0)]
GH_PATH = [(-11.0, -8.5), (-20.0, -9.0), (-27.0, -9.5)]
POND_PATH = [(0.0, 4.0), (-8.0, 9.0), (-16.0, 11.0)]


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Berrybrook", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])

    # --- Cirro: the hill farm ------------------------------------------------------------------
    write_heightmap(os.path.join(PROJECT, "maps", "farm_height.png"))
    layers = ground_layers(pix)
    cir.call("terrain_create", name="Berrybrook Hill", heightmap="maps/farm_height.png", size=TER_SIZE, resolution=TER_RES, layers=layers,
             generator={"minHeight": HMIN, "maxHeight": HMAX, "erosion": 0.0, "thermal": 0.0, "detailNoise": 0.12})
    cir.call("entity_update", entity="Berrybrook Hill", components={"terrain": {"waterLevel": pond_level(), "wetBand": 0.5,
                                                                                "macroVariation": 0.45}})
    lane = [(x, lane_z(x)) for x in range(-150, 151, 3)]
    cir.call("terrain_paint", entity="Berrybrook Hill", layer=LANE, strokes=[dict(x=x, z=z, radius=2.6, strength=1.0, falloff=0.5)
                                                                            for x, z in lane])
    for pts, rad in ((DRIVE, 1.5), (FIELD_PATH, 1.1), (GH_PATH, 1.0), (POND_PATH, 0.9)):
        cir.call("terrain_paint", entity="Berrybrook Hill", layer=LANE,
                 strokes=[dict(x=x, z=z, radius=rad, strength=0.9, falloff=0.6) for x, z in path_points(pts, 0.8)])
    # a mown lawn around the cottage, and wildflower drifts in the meadows
    cir.call("terrain_paint", entity="Berrybrook Hill", layer=LAWN, strokes=[
        dict(x=HOUSE[0] + dx, z=HOUSE[1] + dz, radius=7.5, strength=1.0, falloff=0.6) for dx, dz in ((-6, 4), (0, 6), (6, 4), (-10, 0), (8, -2),
                                                                                                    (2, 12), (-6, 12), (10, 10))])
    rb = rnd(99)
    blooms = []
    while len(blooms) < 34:
        x, z = rb.uniform(-75, 70), rb.uniform(-60, 45)
        if math.hypot(x - FIELD[0], z - FIELD[1]) < 17 or math.hypot(x - HOUSE[0], z - HOUSE[1]) < 16 or abs(z - lane_z(x)) < 5:
            continue
        if math.hypot(x - POND[0], z - POND[1]) < 12 or math.hypot(x - GREENHOUSE[0], z - GREENHOUSE[1]) < 8:
            continue
        blooms.append(dict(x=x, z=z, radius=rb.uniform(2.5, 6.5), strength=1.0, falloff=0.7))
    blooms += [dict(x=x, z=z, radius=r_, strength=1.0, falloff=0.7) for x, z, r_ in ((29, 1, 5.5), (25, -3, 4.0), (33, -4, 4.5), (22, 4, 3.5),
                                                                                     (-8, 14, 4.0), (8, 12, 3.5))]
    cir.call("terrain_paint", entity="Berrybrook Hill", layer=BLOOM, strokes=blooms)
    # the yard in front of the house and around the gameplay plots is worn earth
    cir.call("terrain_paint", entity="Berrybrook Hill", layer=LANE, strokes=[
        dict(x=HOUSE[0] + 1, z=HOUSE[1] + 7.5, radius=4.0, strength=0.6, falloff=0.8),
        dict(x=-2.0, z=-5.5, radius=3.6, strength=0.55, falloff=0.8)])
    # tilled ground under the berry rows
    fx, fz = FIELD
    cir.call("terrain_paint", entity="Berrybrook Hill", layer=TILLED, strokes=[
        dict(x=fx - 8 + 2.0 * i, z=fz - ROWS * ROW_GAP / 2 + ROW_GAP * j, radius=1.6, strength=1.0, falloff=0.4)
        for i in range(9) for j in range(ROWS + 1)])
    # the far hills: a second, coarse terrain filling every horizon
    far_layers = [{"name": "grass", "material": layers[0]["material"], "tiling": 40, "color": "#94c464"},
                  {"name": "meadow", "material": layers[1]["material"], "tiling": 60, "color": "#a9c873", "slopeMin": 14, "noise": 0.6}]
    cir.call("terrain_create", name="Far Hills", preset="rolling_hills", size=5000, resolution=513, seed=7, layers=far_layers,
             position=[0, -6, 0], generator={"maxHeight": 70, "featureSize": 1000})
    cir.call("terrain_sculpt", entity="Far Hills", strokes=[dict(x=0, z=0, radius=420, strength=1, mode="flatten", target=-40, falloff=0.6),
                                                           dict(x=0, z=0, radius=150, strength=1, mode="flatten", target=-40, falloff=0.1)])
    cir.call("terrain_layers", entity="Far Hills", layers=far_layers)
    cir.flush("Hill farm")

    # --- Pixel: the kit ----------------------------------------------------------------------
    meshes = model_kit(pix)
    M = kit_materials(pix)
    farm(studio, meshes, M)
    greenery(studio, meshes, M)
    pond(studio, meshes, M)
    life(studio, meshes, M)
    gameplay(studio, meshes, M)
    nim.e("Player Camera", pos=tuple(META["view"]["eye"]), camera={"fov": 45, "primary": True, "farPlane": 6000, "nearPlane": 0.08})
    nim.flush("Game camera")
    sequences(nim)
    aur.call("environment_update", **MOODS["golden"])
    nim.call("scene_save", path="scenes/main.sky.json")
    write_credits()


def write_credits():
    """CREDITS.md: the Poly Haven scans this scene uses, plus everything the crew made procedurally."""
    lines = ["# Credits", "", "Berrybrook is original work: every model, icon, portrait, line of dialogue and sign was made for this",
             "project. Third-party assets are CC0 photo textures from Poly Haven.", "", "## Third-party assets (CC0)", ""]
    for asset_id in ("leafy_grass", "brown_mud_leaves_01"):
        c = ph._credits(asset_id)
        lines.append(f"- **{asset_id}** by {c['author']} — {c['license']} — {c['source_page']} (`downloads/{asset_id}`)")
    lines += ["", "## Made for Berrybrook (procedural, no third-party content)", "",
              "- Farm kit modelled in Blender 3.5 through Skywalker's DCC bridge by `media/demo/games/berrybrook_blender.py`:",
              "  farmhouse, glasshouse, smock windmill and sails, berry stand, strawberry rows and plants (sprout to ripe),",
              "  fences, crates, hay, wheelbarrow, lily pads, reeds, storybook trees, shrubs, garden flowers, hens, the dog, butterflies.",
              "- HUD icons and Juniper's portrait painted with PIL by `media/demo/games/berrybrook_art.py`.",
              "- Terrain heightmap (`maps/farm_height.png`) computed by `media/demo/games/berrybrook.py`; wood, stone, dirt and",
              "  soil textures generated by the engine's `texture_generate`.",
              "- Fonts: Inter and EB Garamond (SIL Open Font License), built into the engine.", ""]
    with open(os.path.join(PROJECT, "CREDITS.md"), "w") as f:
        f.write("\n".join(lines))


def at(x, z, dy=0.0):
    return (x, round(H(x, z) + dy, 3), z)


def farm(studio, meshes, M):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    r = rnd(2024)
    hx, hz = HOUSE
    hy = -0.06 * (hz + 1.2)
    place_kit(cir, meshes, M, "farmhouse", "Farmhouse", (hx, hy - 0.05, hz), 0, tags=["building"])
    gx, gz = GREENHOUSE
    place_kit(cir, meshes, M, "greenhouse", "Greenhouse", (gx, -0.06 * gz - 0.05, gz), 0, tags=["building"])
    mx, mz = MILL
    place_kit(cir, meshes, M, "windmill", "Windmill", (mx, H_MILL_TOP - 0.1, mz), 0, tags=["building"])
    # the sails turn on the windshaft (Blender hub at (0, -2.75, 10.25 + 1.2) -> engine (0, 11.45, 2.75))
    place_kit(cir, meshes, M, "windmill_sails", "Windmill Sails", (mx, H_MILL_TOP - 0.1 + 11.45, mz + 2.95), 0, tags=["sails"])
    sx, sz = STALL
    place_kit(cir, meshes, M, "market_stall", "Berry Stand", (sx, -0.06 * sz - 0.02, sz), 0, tags=["stall"])
    cir.flush("Farmhouse, greenhouse, windmill and the berry stand")

    # the berry field: rows run east-west, alternating two plantings
    fx, fz = FIELD
    for j in range(ROWS):
        z = fz - (ROWS - 1) * ROW_GAP / 2 + j * ROW_GAP
        key = "strawberry_row" if j % 2 == 0 else "strawberry_row_b"
        place_kit(cir, meshes, M, key, f"Berry Row {j + 1}", (fx + r.uniform(-0.3, 0.3), round(-0.06 * z - 0.05, 3), z),
                  180 if j % 3 == 1 else 0, tags=["berries"])
    cir.flush("Strawberry rows")

    # fences: split rails around the field and along the lane, white pickets around the cottage garden
    def fence_line(key, seg, p0, p1, prefix, dy=-0.05):
        n = max(1, round(math.dist(p0, p1) / seg))
        for k in range(n):
            a = (p0[0] + (p1[0] - p0[0]) * k / n, p0[1] + (p1[1] - p0[1]) * k / n)
            b = (p0[0] + (p1[0] - p0[0]) * (k + 1) / n, p0[1] + (p1[1] - p0[1]) * (k + 1) / n)
            yaw = math.degrees(math.atan2(-(b[1] - a[1]), b[0] - a[0]))
            place_kit(pix, meshes, M, key, f"{prefix} {k + 1}", at(a[0], a[1], dy), yaw, scale=(math.dist(a, b) / seg, 1, 1))
    x0, x1 = fx - 9.5, fx + 9.5
    z0, z1 = fz - ROWS * ROW_GAP / 2 - 1.3, fz + ROWS * ROW_GAP / 2 + 1.0
    fence_line("fence_rail", 3.0, (x0, z0), (x1, z0), "Field Fence N")
    fence_line("fence_rail", 3.0, (x1, z0), (x1, z1), "Field Fence E")
    fence_line("fence_rail", 3.0, (x1, z1), (fx - 6.0, z1), "Field Fence S")
    fence_line("fence_rail", 3.0, (x0, z1 - 3.0), (x0, z0), "Field Fence W")
    fence_line("fence_rail", 3.0, (-60, lane_z(-60) - 3.2), (-6, lane_z(-6) - 3.2), "Lane Fence W")
    fence_line("fence_rail", 3.0, (21, lane_z(21) - 3.2), (60, lane_z(60) - 3.2), "Lane Fence E")
    pix.flush("Split-rail fences")
    gzf = hz + 9.0
    fence_line("fence_picket", 2.4, (hx - 7.5, gzf), (hx - 1.6, gzf), "Garden Fence W", -0.03)
    fence_line("fence_picket", 2.4, (hx + 1.6, gzf), (hx + 7.5, gzf), "Garden Fence E", -0.03)
    fence_line("fence_picket", 2.4, (hx - 7.5, hz + 4.6), (hx - 7.5, gzf), "Garden Fence Side W", -0.03)
    fence_line("fence_picket", 2.4, (hx + 7.5, gzf), (hx + 7.5, hz + 4.6), "Garden Fence Side E", -0.03)
    pix.flush("Picket fence")

    # the farm sign at the lane, crates, hay, a wheelbarrow
    dx, dz = DRIVE[0]
    place_kit(pix, meshes, M, "farm_sign", "Farm Sign", at(dx, dz - 2.6, -0.05), 0)
    pix.op("entity_create", name="Farm Sign Text", parent="Farm Sign", position=[0, 1.8, -0.075], rotation=[0, 0, 0],
           components={"text": {"text": "Berrybrook Farm", "font": "serif", "size": 0.34, "color": "#5b3a24", "align": "center"}})
    pix.op("entity_create", name="Farm Sign Subtitle", parent="Farm Sign", position=[0, 1.57, -0.075],
           components={"text": {"text": "strawberries · jam · honey", "font": "serif", "size": 0.14, "color": "#7a5236"}})
    for k, (x, z, yaw) in enumerate(((fx - 10.8, fz + 6.0, 20), (fx + 10.6, fz - 4, -70), (sx + 2.6, sz + 0.6, 10))):
        place_kit(pix, meshes, M, "crate_stack", f"Berry Crates {k + 1}", at(x, z, -0.02), yaw)
    for k, (x, z, yaw) in enumerate(((fx + 11.5, fz + 4.5, 15), (fx + 12.3, fz + 2.8, 75), (mx - 7, mz + 6, 40), (sx - 3.4, sz - 1.8, -10))):
        place_kit(pix, meshes, M, "hay_bale", f"Hay Bale {k + 1}", at(x, z, -0.04), yaw)
    place_kit(pix, meshes, M, "wheelbarrow", "Wheelbarrow", at(fx - 10.2, fz + 3.6, 0.32), 205)
    pix.flush("Sign, crates, hay and a wheelbarrow")
    pix.op("entity_create", name="Stand Chalkboard Text", parent="Berry Stand", position=[-2.2, 0.66, 0.735], rotation=[-12, 0, 0],
           components={"text": {"text": "FRESH\nBERRIES\n<size=65%>3 coins a basket</size>", "font": "serif", "size": 0.1,
                                "color": "#f4efe2", "lineSpacing": 1.05}})
    pix.flush("Chalkboard")

    # the gameplay plots by the house: tilled patches with plants at every stage
    stages = ["ripe", "flowering", "young", "sprout", "ripe", None]
    for k, (x, z) in enumerate(PLOTS):
        name = f"Plot {k + 1}"
        y = round(-0.06 * PADS[5][1] - 0.01, 3)
        stra.e(name, meshes["soil_patch"]["soil"], pos=(x, y, z), material=M["soil"], tags=["plot"],
               vars={"stage": {"ripe": 4, "flowering": 3, "young": 2, "sprout": 1, None: 0}[stages[k]]})
        stra.e(f"{name} stones", meshes["soil_patch"]["stone"], parent=name, material=M["stone"])
        for st in ("sprout", "young", "flowering", "ripe"):
            stra.e(f"{name} {st}", pos=(0, 0.07, 0), parent=name, tags=["plot_stage"])
            for j, (ox, oz) in enumerate(((-0.27, -0.27), (0.27, -0.26), (-0.26, 0.27), (0.28, 0.28))):
                plant = f"{name} {st} {j + 1}"
                stra.e(plant, pos=(ox, 0.0, oz), parent=f"{name} {st}", rot=(0, r.uniform(0, 360), 0), scale=r.uniform(1.15, 1.35))
                for part, mesh in meshes[f"plant_{st}"].items():
                    stra.e(f"{plant} {part}", mesh, parent=plant, material=M[part])
    stra.flush("Gameplay plots")

    # warm light: windows, porch lantern, fairy lights, the greenhouse, the mill
    for k, (x, y, z) in enumerate(((-2.75, 2.1, 2.4), (1.65, 2.1, 2.4), (3.2, 2.1, 2.4), (-0.6, 2.2, 2.4), (0.9, 4.6, 1.6))):
        aur.light(f"Window Glow {k + 1}", (hx + x, hy + y, hz + z), "#ffb066", 2.2, 5.0, tags=["window_light"])
    aur.light("Porch Lantern", (hx + 0.25, hy + 2.25, hz + 3.75), "#ffa85a", 3.0, 6.0, tags=["flicker"])
    for k in range(4):
        aur.light(f"Fairy Light {k + 1}", (hx - 3.6 + 2.4 * k, hy + 2.85, hz + 5.4), "#ffc27a", 1.3, 3.5, tags=["fairy"])
    aur.light("Greenhouse Glow", (gx, -0.06 * gz + 2.0, gz), "#ffd59a", 1.6, 6.0)
    aur.light("Mill Window", (mx, H_MILL_TOP + 5.8, mz + 2.6), "#ffb066", 1.4, 4.0)
    aur.flush("Warm lights")
    aur.call("fx_create", effect="smoke", name="Chimney Smoke", position=[hx - 2.6, hy + 8.3, hz - 1.0],
             overrides={"rate": 3, "lifetime": 7, "sizeStart": 0.35, "sizeEnd": 2.6, "speed": 0.7, "gravity": -0.15,
                        "colorStart": "#e9e2da70", "colorEnd": "#d9d2cc00", "maxParticles": 40, "wind": 0.6, "prewarm": True})


# =============================================================================================
# Greenery: storybook trees, bushes, flower beds, meadow grass
# =============================================================================================
def kit_prefab(b, meshes, M, key):
    """A multi-part kit model as a prefab (for foliage layers and scattering)."""
    path = f"prefabs/{key}.prefab.json"
    place_kit(b, meshes, M, key, f"Prefab {key}", (0, -500, 0), 0)
    b.flush(f"Prefab {key}")
    b.prefab(f"Prefab {key}", path, f"Berrybrook {key.replace('_', ' ')} (procedural, Blender)", tags=["berrybrook"])
    return path


def greenery(studio, meshes, M):
    pix, cir = studio.agent("Pixel"), studio.agent("Cirro")
    P = {k: kit_prefab(pix, meshes, M, k) for k in ("tree_a", "tree_b", "tree_c", "tree_poplar", "tree_blossom", "tree_apple", "bush", "bush_hydrangea",
                                                    "bush_rose", "reeds")}
    # keep foliage off the buildings: paint worn earth under their footprints
    hx, hz = HOUSE
    gx, gz = GREENHOUSE
    mx, mz = MILL
    sx, sz = STALL
    cir.call("terrain_paint", entity="Berrybrook Hill", layer=LANE, strokes=[
        dict(x=hx, z=hz + 1.0, radius=6.2, strength=1.0, falloff=0.15), dict(x=hx - 3, z=hz + 1.0, radius=5.5, strength=1.0, falloff=0.15),
        dict(x=hx + 3, z=hz + 1.0, radius=5.5, strength=1.0, falloff=0.15),
        dict(x=gx, z=gz - 2, radius=3.4, strength=1.0, falloff=0.2), dict(x=gx, z=gz + 2, radius=3.4, strength=1.0, falloff=0.2),
        dict(x=mx, z=mz, radius=4.2, strength=1.0, falloff=0.3), dict(x=sx, z=sz, radius=2.6, strength=1.0, falloff=0.3)])

    def L(res_or_mesh, **fields):
        layer = {"prefab": res_or_mesh} if res_or_mesh.endswith(".prefab.json") else {"mesh": res_or_mesh}
        layer.update(fields)
        return layer

    cir.call("foliage_add", entity="Berrybrook Hill", name="Meadow", seed=5, layers=[
        {"preset": "meadow_grass", "density": 28, "color": "#6f9f3c", "terrainLayer": GRASS, "cullDistance": 85, "colorVariation": 0.45,
         "scaleMin": 0.7, "scaleMax": 1.2},
        {"preset": "meadow_grass", "density": 15, "color": "#7fa743", "terrainLayer": MEADOW, "cullDistance": 95, "colorVariation": 0.5},
        {"preset": "meadow_grass", "density": 60, "color": "#7fae45", "terrainLayer": LAWN, "cullDistance": 70, "colorVariation": 0.3,
         "scaleMin": 0.4, "scaleMax": 0.62, "seed": 9},
        {"preset": "meadow_grass", "density": 14, "color": "#7aa443", "terrainLayer": BLOOM, "cullDistance": 85, "colorVariation": 0.4,
         "scaleMin": 0.6, "scaleMax": 1.0, "seed": 10},
        {"preset": "flowers", "density": 3.0, "terrainLayer": BLOOM, "cullDistance": 90, "seed": 11},
        {"preset": "tall_grass", "density": 1.2, "color": "#93a648", "terrainLayer": MEADOW, "cullDistance": 110, "clumping": 0.85},
        {"preset": "tall_grass", "density": 0.5, "color": "#8aa54a", "terrainLayer": GRASS, "cullDistance": 90, "clumping": 0.95, "seed": 3},
        {"preset": "flowers", "density": 0.5, "terrainLayer": GRASS, "cullDistance": 70},
        {"preset": "flowers", "density": 0.9, "terrainLayer": MEADOW, "cullDistance": 70, "seed": 6},
    ])
    # Blender flower clumps (two parts each: stems + petals) and bushes/trees as prefab layers
    clumps = []
    for i, k in enumerate(("flowers_daisy", "flowers_cosmos", "flowers_butter", "flowers_lilac", "flowers_tulip")):
        pf = kit_prefab(pix, meshes, M, k)
        clumps.append(L(pf, density=0.04 if k != "flowers_tulip" else 0.015, scaleMin=0.8, scaleMax=1.3,
                        slopeMax=25, terrainLayer=GRASS, clumping=0.92, cullDistance=60, wind=1.0, castShadows=False, seed=30 + i,
                        color="#ffffff", subsurface=0.5, alignToNormal=0.3))
        clumps.append(L(pf, density=0.5, scaleMin=0.9, scaleMax=1.4, slopeMax=28, terrainLayer=BLOOM, clumping=0.6, cullDistance=90,
                        wind=1.0, castShadows=False, seed=40 + i, color="#ffffff", subsurface=0.5, alignToNormal=0.3))
    cir.call("foliage_add", entity="Berrybrook Hill", name="Wildflowers", seed=8, layers=clumps)
    cir.call("terrain_paint", entity="Berrybrook Hill", layer=GRASS, strokes=[
        dict(x=MILL[0] + dx, z=MILL[1] + dz, radius=13, strength=1.0, falloff=0.5) for dx, dz in ((0, 0), (-10, 10), (10, 8), (-4, 18))])
    tree = dict(slopeMax=32, cullDistance=1600, wind=0.25, color="#ffffff", subsurface=0.45, alignToNormal=0.0, randomTilt=2)
    cir.call("foliage_add", entity="Berrybrook Hill", name="Woods", seed=12, layers=[
        L(P["tree_a"], density=0.0045, scaleMin=0.9, scaleMax=1.5, terrainLayer=MEADOW, clumping=0.97, heightMin=4.0, **tree),
        L(P["tree_c"], density=0.003, scaleMin=0.9, scaleMax=1.4, terrainLayer=MEADOW, clumping=0.97, seed=2, heightMin=4.0, **tree),
        L(P["tree_b"], density=0.0015, scaleMin=0.8, scaleMax=1.3, terrainLayer=MEADOW, clumping=0.9, seed=3, **tree),
        L(P["tree_b"], density=0.0006, scaleMin=0.8, scaleMax=1.2, terrainLayer=GRASS, clumping=0.3, seed=5, heightMin=2.0, **tree),
        L(P["bush"], density=0.012, scaleMin=0.8, scaleMax=1.6, slopeMax=30, terrainLayer=MEADOW, clumping=0.85, cullDistance=300, wind=0.4,
          color="#ffffff", subsurface=0.45, alignToNormal=0.2, seed=4),
    ])
    cir.call("foliage_add", entity="Far Hills", name="Far Woods", seed=21, layers=[
        L(P["tree_a"], density=0.0016, scaleMin=1.2, scaleMax=2.0, terrainLayer=-1, clumping=0.97, heightMin=-20, **dict(tree, cullDistance=3000)),
        L(P["tree_c"], density=0.001, scaleMin=1.2, scaleMax=2.0, terrainLayer=-1, clumping=0.97, heightMin=-20, seed=3,
          **dict(tree, cullDistance=3000)),
    ])

    # hand-placed: trees framing the farm, an orchard by the pond, hedges and flower beds by the house
    r = rnd(77)
    trees = [("tree_a", -24, -30, 1.3), ("tree_b", -8, -31, 1.15), ("tree_a", -44, -22, 1.4), ("tree_b", -40, -2, 1.2),
             ("tree_blossom", -4, -24, 1.0), ("tree_a", 46, -14, 1.25), ("tree_b", 25, 17, 1.1), ("tree_blossom", 22, 14, 0.95),
             ("tree_apple", -38, 8, 1.0), ("tree_apple", -33, 22, 0.95), ("tree_apple", -42, 18, 1.05), ("tree_blossom", -12, 26, 1.0),
             ("tree_a", 46, 20, 1.3), ("tree_b", -58, 12, 1.4), ("tree_a", 52, -26, 1.2), ("tree_b", 20, -38, 1.1)]
    for k, x in enumerate(range(-120, 121, 9)):
        if -14 < x < 30:
            continue  # open view onto the farm from the lane
        trees.append(("tree_poplar", x + r.uniform(-1.5, 1.5), lane_z(x) + 4.2 + r.uniform(-0.6, 0.6), r.uniform(0.85, 1.15)))
    for k, (key, x, z, s) in enumerate(trees):
        pix.op("prefab_instantiate", prefab=P[key], name=f"Tree {k + 1}", position=list(at(x, z, -0.1)), yaw=r.uniform(0, 360), scale=s)
    pix.flush("Trees around the farm")
    for k, (x, z) in enumerate(((gx - 2.6, gz + 4.4), (gx + 2.6, gz + 4.4), (sx + 2.4, sz - 1.4), (-6.5, -3.5), (1.2, -3.2),
                                (mx - 4.5, mz + 4.5), (mx + 4.2, mz + 3.5))):
        pix.op("prefab_instantiate", prefab=P["bush_hydrangea" if k % 2 else "bush_rose"], name=f"Bush {k + 1}",
               position=list(at(x, z, -0.05)), yaw=r.uniform(0, 360), scale=r.uniform(0.8, 1.1))
    pix.flush("Hedges and flower bushes")
    # flower beds in the cottage garden: soil beds along the picket fence and under the porch rail, packed with flowers
    beds = []
    for z in (hz + 8.15, hz + 6.0):
        for x in [hx - 6.8 + 0.5 * k for k in range(28)]:
            if abs(x - (hx - 0.6)) < 1.25:
                continue  # the garden path
            beds.append(dict(x=x, z=z, radius=0.75, strength=1.0, falloff=0.35))
    for z in [hz + 5.6 + 0.5 * k for k in range(6)]:
        for x in (hx - 6.9, hx + 6.9):
            beds.append(dict(x=x, z=z, radius=0.6, strength=1.0, falloff=0.35))
    cir.call("terrain_paint", entity="Berrybrook Hill", layer=BED, strokes=beds)
    garden = []
    for i, k in enumerate(("flowers_daisy", "flowers_cosmos", "flowers_tulip", "flowers_butter", "flowers_lilac")):
        garden.append(L(f"prefabs/{k}.prefab.json", density=2.6 if k != "flowers_tulip" else 1.2, scaleMin=0.85, scaleMax=1.25, slopeMax=40,
                        terrainLayer=BED, clumping=0.75,
                        cullDistance=120, wind=0.9, castShadows=True, seed=60 + i, color="#ffffff", subsurface=0.5, alignToNormal=0.1,
                        layerThreshold=0.5))
    garden.append(L(P["bush_rose"], density=0.12, scaleMin=0.5, scaleMax=0.75, terrainLayer=BED, clumping=0.5, cullDistance=200, wind=0.4,
                    color="#ffffff", subsurface=0.4, alignToNormal=0.1, seed=70, layerThreshold=0.5))
    cir.call("foliage_add", entity="Berrybrook Hill", name="Cottage Garden", seed=14, layers=garden)


def pond(studio, meshes, M):
    pix, aur = studio.agent("Pixel"), studio.agent("Aurora")
    px, pz, pr = POND
    wl = pond_level()
    aur.call("fx_create", effect="lake", name="Lily Pond", position=[px, wl, pz],
             overrides={"size": pr * 2.6, "windSpeed": 0.8, "waveScale": 0.05, "choppiness": 0.15, "patchSize": 12, "depth": 1.5,
                        "deepColor": "#1d3b2c", "shallowColor": "#5d8a6a", "clarity": 1.4, "foam": 0.0, "roughness": 0.02})
    r = rnd(303)
    for k, (dx, dz, s) in enumerate(((-3.0, -2.0, 1.6), (2.5, 1.5, 1.4), (-1.0, 3.5, 1.3), (4.0, -3.0, 1.2), (-5.5, 1.5, 1.3),
                                     (5.5, 2.5, 1.1), (0.5, -4.5, 1.2), (-6.0, -3.5, 1.0), (1.5, 5.5, 1.0))):
        place_kit(pix, meshes, M, "lily_pads", f"Lily Pads {k + 1}", (px + dx, wl + 0.005, pz + dz), r.uniform(0, 360), scale=s,
                  shadows=False, tags=["lily"])
    for k in range(30):
        a = r.uniform(0, 2 * math.pi)
        d = pr * r.uniform(0.9, 1.06)
        x, z = px + math.cos(a) * d * 1.15, pz + math.sin(a) * d
        if 0.2 < a < 1.3:
            continue  # leave the path end open
        pix.op("prefab_instantiate", prefab="prefabs/reeds.prefab.json", name=f"Reeds {k + 1}", position=[x, wl - 0.05, z],
               yaw=r.uniform(0, 360), scale=r.uniform(0.8, 1.3))
    pix.flush("Lily pads and reeds")


# =============================================================================================
# Life: hens, a sleepy dog, butterflies, the turning windmill, flickering lamps, drifting petals
# =============================================================================================
HEN = """behavior Hen
  intent "Potter about the yard: amble to a nearby spot, stop and peck at the ground a few times, then wander on, never far from home."
  param speed = 0.5 in 0..2 "walk speed (m/s)"
  param roam = 2.6 in 0..8 "how far from home the hen wanders (m)"
  var home = (0, 0, 0)
  var goal = (0, 0, 0)
  var yaw = 0
  on start
    home = self.position
    goal = home
    yaw = self.rotation.y
  end
  state Peck
    on tick
      let p = max(0, sin(state_time * 8 + self.id))
      self.rotation = (p * 28, yaw, 0)
      if state_time > 1.2 + abs(noise(self.id * 3.1 + time * 0.1)) * 2.5 then
        go to Walk
      end
    end
  end
  state Walk
    on enter
      goal = home + (random(0 - roam, roam), 0, random(0 - roam, roam))
    end
    on tick
      let d = goal - self.position
      if d.length < 0.1 or state_time > 9 then
        go to Peck
      end
      let want = atan2(d.x, d.z) * 57.2958
      let turn = (want - yaw + 540) % 360 - 180
      yaw = yaw + turn * min(1, dt * 6)
      move self toward goal at speed
      self.rotation = (sin(time * 16 + self.id) * 3, yaw, sin(time * 8 + self.id) * 4)
    end
  end
end"""

DOG = """behavior Snooze
  intent "A sleepy dog curled up on the porch: slow breathing, an ear twitch now and then."
  var base = (1, 1, 1)
  on start
    base = self.scale
  end
  on tick
    let b = sin(time * 1.5) * 0.025
    self.scale = (base.x * (1 + b * 0.5), base.y * (1 + b), base.z)
  end
end"""

BUTTERFLY = """behavior Flutter
  intent "Drift in lazy loops over the flowers, bobbing up and down and turning to face where it is going."
  var base = (0, 0, 0)
  on start
    base = self.position
  end
  on tick
    let t = time * self.speed + self.phase
    let p = base + (sin(t) * self.r + sin(t * 2.3) * 0.35, 0.35 + sin(t * 1.7) * 0.3 + abs(sin(time * 7 + self.phase)) * 0.06, cos(t * 0.8) * self.r * 0.8 + sin(t * 1.9) * 0.3)
    let v = p - self.position
    self.position = p
    if v.length > 0.0001 then
      self.rotation = (0, atan2(v.x, v.z) * 57.2958, sin(t * 2.0) * 10)
    end
  end
end"""

WING = """on tick
  self.rotation = (0, 0, (sin(time * self.beat + self.phase) * 52 + 18) * self.side)
end"""

SAILS = """behavior Sails
  intent "The windmill's sails turn slowly in the evening breeze."
  param rpm = 3.2 in 0..20 "revolutions per minute"
  on tick
    self.rotation = (0, 0, 0 - time * rpm * 6)
  end
end"""

FLICKER = """behavior Flicker
  var base = 0
  on start
    base = self.light.intensity
  end
  on tick
    let n = sin(time * 7.3 + self.id) * 0.5 + sin(time * 12.9 + self.id * 1.7) * 0.3 + sin(time * 23.0 + self.id * 0.3) * 0.2
    self.light.intensity = base * (1 + n * 0.12)
  end
end"""

TWINKLE = """behavior Twinkle
  var base = 0
  on start
    base = self.light.intensity
  end
  on tick
    self.light.intensity = base * (0.85 + 0.15 * sin(time * 2.1 + self.id * 1.3))
  end
end"""

GLIDE = """behavior Glide
  intent "Swallows wheel and swoop on the evening air, banking into their turns."
  on tick
    let a = time * self.speed + self.phase
    self.position = (self.cx + cos(a) * self.r + sin(a * 2.7) * 1.5, self.h + sin(a * 2.3) * 2.0, self.cz + sin(a) * self.r)
    self.rotation = (sin(a * 2.3) * 8, 0 - a * 57.2958 + (90 - 90 * sign(self.speed)), 0 - 22 * sign(self.speed))
  end
end"""

FLAP = """on tick
  let beat = sin(time * self.beat + self.phase)
  self.rotation = (0, 0, (beat * 40 + 8) * self.side)
end"""

WING_COLORS = ["#f29a3a", "#f4d35e", "#7fb8e8", "#fbf3e4", "#f2a0b5", "#f6b860"]


def life(studio, meshes, M):
    pix, aur, stra = (studio.agent(n) for n in ("Pixel", "Aurora", "Stratus"))
    r = rnd(4242)
    hx, hz = HOUSE
    # hens in the yard and by the stand, a rooster-proud white flock
    flock = [(-6.0, -9.5), (-4.5, -11.0), (-7.5, -12.0), (-1.0, -10.5), (2.0, -2.5), (-9.5, -6.0), (STALL[0] - 2.0, STALL[1] + 2.4),
             (STALL[0] + 1.5, STALL[1] + 3.2), (-3.0, 1.5)]
    for k, (x, z) in enumerate(flock):
        key = "hen" if k % 3 else "chicken"
        place_kit(stra, meshes, M, key, f"Hen {k + 1}", at(x, z, -0.01), r.uniform(0, 360), scale=r.uniform(1.25, 1.4), tags=["hen"])
    # the dog asleep on the porch
    place_kit(stra, meshes, M, "dog", "Biscuit", (hx - 0.1, -0.06 * (hz + 1.2) + 0.52, hz + 4.55), 20, scale=1.15, tags=["dog"])
    stra.flush("Hens and the dog")
    for k in range(len(flock)):
        stra.behave(f"Hen {k + 1}", "Hen", "Potter about, peck at the ground, wander on.", HEN)
    stra.behave("Biscuit", "Snooze", "Slow sleepy breathing.", DOG)
    stra.behave("Windmill Sails", "Sails", "The sails turn slowly in the breeze.", SAILS)
    stra.flush("Hen and dog behaviors")

    # butterflies over the garden, the field and the pond
    spots = [(hx - 3.5, hz + 7.0), (hx + 4.0, hz + 6.8), (hx, hz + 8.0), (FIELD[0] - 4, FIELD[1] + 2), (FIELD[0] + 3, FIELD[1] - 2),
             (FIELD[0] - 7, FIELD[1] - 5), (POND[0] + 4, POND[1] - 6), (-2.5, -5.0), (STALL[0] - 1, STALL[1] + 1.5), (-20, -6)]
    for k, (x, z) in enumerate(spots):
        name = f"Butterfly {k + 1}"
        col = WING_COLORS[k % len(WING_COLORS)]
        stra.e(name, pos=at(x, z, 0.5), scale=1.7, tags=["butterfly"],
               vars={"speed": r.uniform(0.35, 0.6), "phase": r.uniform(0, 6.28), "r": r.uniform(0.9, 1.8)})
        stra.e(f"{name} body", meshes["butterfly_body"]["body"], parent=name, material=M["body"])
        for side, sgn in (("R", 1), ("L", -1)):
            stra.e(f"{name} wing {side}", meshes["butterfly_wing"]["wing"], parent=name, scale=(sgn, 1, 1), material=M["wing"],
                   color=col, castShadows=False, vars={"side": sgn, "beat": r.uniform(16, 22), "phase": r.uniform(0, 6.28)})
    stra.flush("Butterflies")
    for k in range(len(spots)):
        stra.behave(f"Butterfly {k + 1}", "Flutter", "Lazy loops over the flowers.", BUTTERFLY)
        for side in ("R", "L"):
            stra.behave(f"Butterfly {k + 1} wing {side}", "Wingbeat", "Quick wingbeats.", WING)
    stra.flush("Butterfly flight")
    for e in stra.call("scene_query", tag="flicker")["matches"]:
        stra.behave(e["name"], "Flicker", "A candle-like flicker.", FLICKER)
    for e in stra.call("scene_query", tag="fairy")["matches"]:
        stra.behave(e["name"], "Twinkle", "Fairy lights breathe softly.", TWINKLE)
    stra.flush("Lamps")

    # swallows wheeling over the windmill knoll and the farmyard
    flights = [(MILL[0], MILL[1], 16, H_MILL_TOP + 16), (MILL[0] - 6, MILL[1] + 6, 24, H_MILL_TOP + 22), (-6, -12, 22, 14), (8, -20, 30, 18)]
    k = 0
    for cx, cz, rad, h in flights:
        for _ in range(3):
            name = f"Swallow {k + 1}"
            speed = (8.0 + r.uniform(-1.5, 1.5)) / rad * (1 if k % 4 else -1)
            stra.e(name, pos=(cx + rad, h, cz), tags=["bird"], scale=2.2,
                   vars={"cx": cx, "cz": cz, "r": rad + r.uniform(-4, 4), "h": h + r.uniform(-3, 3), "speed": speed,
                         "phase": k * 0.9 + r.uniform(0, 0.5)})
            stra.e(f"{name} body", meshes["bird_body"]["feather"], parent=name, material=M["feather"])
            stra.e(f"{name} beak", meshes["bird_body"]["beak"], parent=name, material=M["beak"])
            for side, sgn in (("R", 1), ("L", -1)):
                stra.e(f"{name} wing {side}", meshes["bird_wing"]["wing"], parent=name, pos=(0, 0.004, -0.005), scale=(sgn, 1, 1),
                       material=M["wing"], color="#ffffff", vars={"side": sgn, "beat": r.uniform(14, 18), "phase": r.uniform(0, 6.28)})
            k += 1
    stra.flush("Swallows")
    for i in range(k):
        stra.behave(f"Swallow {i + 1}", "Glide", "Wheel on the evening air.", GLIDE)
        for side in ("R", "L"):
            stra.behave(f"Swallow {i + 1} wing {side}", "Flap", "Quick wingbeats.", FLAP)
    stra.flush("Swallow flight")

    # petals drifting from the blossom trees, pollen and dust motes glinting in the low sun
    for k, (x, z) in enumerate(((-4, -24), (22, 14), (-12, 26))):
        aur.call("fx_create", effect="snow", name=f"Blossom Petals {k + 1}", position=list(at(x, z, 6.0)),
                 overrides={"rate": 6, "lifetime": 12, "shapeSize": [7, 0, 7], "sizeStart": 0.05, "sizeEnd": 0.045, "speed": 0.3,
                            "gravity": 0.1, "turbulence": 1.2, "turbulenceScale": 3, "colorStart": "#f6c6d4f0", "colorEnd": "#f2b3c8e0",
                            "floorHeight": H(x, z), "maxParticles": 90, "wind": 0.8, "prewarm": True})
    aur.call("fx_create", effect="dust", name="Golden Motes", position=list(at(2, -6, 1.5)),
             overrides={"rate": 14, "lifetime": 9, "shapeSize": [40, 3, 34], "sizeStart": 0.02, "sizeEnd": 0.015, "speed": 0.08,
                        "gravity": -0.01, "turbulence": 0.6, "colorStart": "#ffe2a080", "colorEnd": "#ffd08000", "maxParticles": 160,
                        "prewarm": True, "wind": 0.3})
    aur.call("fx_create", effect="fireflies", name="Pond Fireflies", position=[POND[0], pond_level() + 0.8, POND[1]],
             overrides={"shapeSize": [12, 1.2, 10], "maxParticles": 40, "rate": 4})


# =============================================================================================
# Gameplay: plant, grow, harvest; a cozy HUD; a chat with Juniper over the berry stand
# =============================================================================================
PLOT = """behavior Plot
  intent "A soil patch: click it to plant a strawberry seed; the plant sprouts, grows, flowers and ripens over time; click a ripe plant to harvest it for coins (the patch is then bare again)."
  param grow_time = 8 in 1..120 "seconds per growth stage"
  param price = 12 in 1..100 "coins for a ripe plant"
  var stage = 0
  var timer = 0
  fn show()
    for i, n in ["sprout", "young", "flowering", "ripe"]
      let e = find(self.name + " " + n)
      if e then
        e.enabled = stage == i + 1
      end
    end
  end
  on start
    show()
  end
  on click
    if stage == 0 then
      stage = 1
      timer = 0
      show()
      emit "planted" with {plot: self.name} to find("HUD")
    elif stage == 4 then
      stage = 0
      timer = 0
      show()
      emit "harvest" with {coins: price, plot: self.name} to find("HUD")
    end
  end
  on tick
    if stage >= 1 and stage < 4 then
      timer += dt
      if timer >= grow_time then
        timer = 0
        stage += 1
        show()
      end
    end
  end

  test "planting starts a sprout"
    stage = 0
    click self
    wait frames 2
    expect stage == 1
  end
  test "a plant ripens over time"
    stage = 1
    timer = 0
    wait grow_time * 3 + 0.5
    expect stage == 4
  end
  test "harvesting clears the patch"
    stage = 4
    click self
    wait frames 2
    expect stage == 0
  end
end"""

HUD = """behavior FarmHud
  intent "The farm HUD: an evening clock that ticks on, the coin purse, and a little toast when berries are sold or seeds planted."
  var coins = 1240
  var minutes = 1120
  var toast_time = 0
  fn clock() -> string
    let h = floor(minutes / 60) % 24
    let m = floor(minutes % 60)
    let suffix = "am"
    if h >= 12 then
      suffix = "pm"
    end
    let h12 = h % 12
    if h12 == 0 then
      h12 = 12
    end
    let mm = str(m)
    if m < 10 then
      mm = "0" + mm
    end
    return str(h12) + ":" + mm + " " + suffix
  end
  on start
    find("Coin Count").ui.text = str(coins)
    find("Clock Time").ui.text = clock() + "  ·  golden hour"
  end
  on tick
    minutes += dt * 0.5
    find("Clock Time").ui.text = clock() + "  ·  golden hour"
    if toast_time > 0 then
      toast_time -= dt
      find("Toast").ui.visible = toast_time > 0
    end
  end
  on event "harvest" with h
    coins += h.get("coins", 0)
    find("Coin Count").ui.text = str(coins)
    find("Toast").ui.text = "+" + str(h.get("coins", 0)) + " coins  ·  sweet!"
    find("Toast").ui.visible = true
    toast_time = 2.5
  end
  on event "planted"
    find("Toast").ui.text = "A seed is tucked in. Grow, little one!"
    find("Toast").ui.visible = true
    toast_time = 2.0
  end
  on event "hud_hide"
    for n in ["Clock Panel", "Purse", "Hotbar", "Hint", "Toast"]
      find(n).ui.visible = false
    end
  end
  on event "hud_show"
    for n in ["Clock Panel", "Purse", "Hotbar", "Hint"]
      find(n).ui.visible = true
    end
  end
end"""

TALK = """behavior Neighbour
  intent "Click Juniper's berry stand to chat with her."
  on click
    if not find("Juniper").dialogue.running then
      start_dialogue(find("Juniper"), "Start")
    end
  end
end"""

STORY = """behavior Story
  intent "Juniper's gift: when the dialogue gives jam, a jar lands in the hotbar."
  on dialogue "give_jam"
    find("Slot 6 Count").ui.text = "4"
    emit "harvest" with {coins: 0} to find("HUD")
  end
end"""

DIALOGUE = """title: Start
---
<<declare $traded = false>>
Juniper: Evening, neighbour! Your berries are blushing redder than my cheeks. #portrait:juniper
Juniper: Grandma Wren always said the last light of the day makes the sweetest picking. #portrait:juniper
-> Want a basket? They're still warm from the sun.
    <<set $traded = true>>
    Juniper: Oh, you darling. Here, a jar of my rhubarb-and-honey jam for your shelf. #portrait:juniper
    <<give_jam>>
-> How's the bakery?
    Juniper: Busy as a hive! Tomorrow's shortcake needs twelve baskets... and a little luck. #portrait:juniper
Juniper: Mind the hens on your way home. They've been plotting something by the pond. #portrait:juniper
===
"""

STYLE_SHEET = {
    "format": "skywalker.uistyle", "extends": "light",
    "vars": {"cream": "#fff7e8", "ink": "#5a3a28", "berry": "#e0525c", "leaf": "#6aa553", "honey": "#d9a35f"},
    "rules": {
        "canvas": {"font": "serif", "fontSize": 22, "color": "$ink"},
        ".cozy": {"background": "#fff8ebf2", "background2": "#fbebd2f2", "radius": 18, "borderWidth": 3, "borderColor": "#c99568",
                  "padding": [10, 18], "shadowColor": "#5a3a2850", "shadowOffset": [0, 6], "shadowBlur": 18},
        ".day": {"font": "serif", "fontSize": 30, "bold": True, "color": "$ink"},
        ".time": {"font": "Inter", "fontSize": 17, "color": "#8a6448", "letterSpacing": 0.04},
        ".coins": {"font": "serif", "fontSize": 36, "bold": True, "color": "#b0662c"},
        ".slot": {"background": "#fff6e6f0", "background2": "#f6e2c4f0", "radius": 14, "borderWidth": 3, "borderColor": "#caa07a",
                  "shadowColor": "#5a3a2840", "shadowOffset": [0, 4], "shadowBlur": 10},
        ".slot_on": {"background": "#fffaf0", "background2": "#ffe9c2", "radius": 14, "borderWidth": 4, "borderColor": "#e0525c",
                     "shadowColor": "#e0525c80", "shadowOffset": [0, 3], "shadowBlur": 16},
        ".count": {"font": "Inter", "fontSize": 15, "bold": True, "color": "#ffffff", "textOutline": 0.25, "textOutlineColor": "#5a3a28"},
        ".key": {"font": "Inter", "fontSize": 12, "bold": True, "color": "#a07a5a"},
        ".hint": {"font": "Inter", "fontSize": 18, "bold": True, "color": "#fff8ec", "textShadowColor": "#3a2416e0", "textShadowOffset": [0, 2],
                  "textOutline": 0.12, "textOutlineColor": "#5a3a2890"},
        ".toast": {"background": "#6aa553ee", "background2": "#5b9447ee", "radius": 20, "padding": [8, 22], "font": "serif",
                   "fontSize": 22, "color": "#ffffff", "shadowColor": "#2a401c60", "shadowOffset": [0, 4], "shadowBlur": 12},
        ".dialogue_box": {"background": "#fff8ebf4", "background2": "#fbe9cff4", "radius": 22, "borderWidth": 3, "borderColor": "#c99568",
                          "shadowColor": "#5a3a2860", "shadowOffset": [0, 8], "shadowBlur": 28},
        ".dialogue_portrait": {"radius": 80, "borderWidth": 3, "borderColor": "#e0525c", "background": "#fde9d2", "imageFit": "cover"},
        ".dialogue_name": {"font": "serif", "fontSize": 24, "bold": True, "color": "#e0525c", "letterSpacing": 0.06},
        ".dialogue_text": {"font": "serif", "fontSize": 28, "lineSpacing": 1.22, "color": "$ink", "verticalAlign": "top"},
        ".dialogue_choice": {"background": "#fff3dff0", "background2": "#f7e1c0f0", "borderColor": "#caa07a", "borderWidth": 2,
                             "radius": 14, "font": "serif", "fontSize": 22, "color": "$ink", "textAlign": "left", "padding": [8, 18],
                             "hover": {"borderColor": "#e0525c"}},
        ".dialogue_hint": {"font": "Inter", "fontSize": 13, "color": "#a07a5a", "textAlign": "right"},
    },
}

HOTBAR = [("seeds", "24"), ("strawberry", "38"), ("watering_can", ""), ("hoe", ""), ("basket", "6"), ("jam", "3"), ("flower", "9"),
          (None, "")]


def gameplay(studio, meshes, M):
    nim, stra = studio.agent("Nimbus"), studio.agent("Stratus")
    import berrybrook_art as art
    art.paint_all(PROJECT)
    os.makedirs(os.path.join(PROJECT, "story"), exist_ok=True)
    os.makedirs(os.path.join(PROJECT, "ui"), exist_ok=True)
    with open(os.path.join(PROJECT, "story", "juniper.dialogue"), "w") as f:
        f.write(DIALOGUE)
    with open(os.path.join(PROJECT, "ui", "berrybrook.uistyle.json"), "w") as f:
        json.dump(STYLE_SHEET, f, indent=2)
    nim.call("dialogue_check", path="story/juniper.dialogue")

    for k in range(len(PLOTS)):
        stra.behave(f"Plot {k + 1}", "Plot", "Plant, grow and harvest strawberries.", PLOT)
    stra.behave("Berry Stand deck", "Neighbour", "Click the stand to chat with Juniper.", TALK)
    stra.flush("Plot and stand behaviors")

    sheet = "ui/berrybrook.uistyle.json"
    slots = []
    for k, (icon, count) in enumerate(HOTBAR):
        kids = [{"type": "text", "name": f"Slot {k + 1} Key", "text": str(k + 1), "style": "key", "anchor": "top_left", "position": [8, 4]}]
        if icon:
            kids.append({"type": "image", "name": f"Slot {k + 1} Icon", "image": f"ui/icons/{icon}.png", "size": [54, 54], "anchor": "center"})
        kids.append({"type": "text", "name": f"Slot {k + 1} Count", "text": count, "style": "count", "anchor": "bottom_right",
                     "position": [-7, -4]})
        slots.append({"type": "panel", "name": f"Slot {k + 1}", "size": [76, 76], "style": "slot_on" if k == 0 else "slot", "children": kids})
    stra.call("ui_create", canvas={"name": "HUD", "theme": "light", "styleSheet": sheet, "sortOrder": 10}, elements=[
        {"type": "panel", "name": "Clock Panel", "anchor": "top_left", "position": [36, 30], "style": "cozy", "layout": "row", "gap": 14,
         "align": "center", "fit": "both", "children": [
             {"type": "image", "name": "Sun Icon", "image": "ui/icons/sun.png", "size": [56, 56]},
             {"type": "panel", "name": "Clock Column", "layout": "column", "gap": 2, "fit": "both", "children": [
                 {"type": "text", "name": "Day Label", "text": "Summer  ·  Day 12", "style": "day", "fit": "both"},
                 {"type": "text", "name": "Clock Time", "text": "6:40 pm  ·  golden hour", "style": "time", "fit": "both"}]}]},
        {"type": "panel", "name": "Purse", "anchor": "top_right", "position": [-36, 30], "style": "cozy", "layout": "row", "gap": 10,
         "align": "center", "fit": "both", "children": [
             {"type": "image", "name": "Coin Icon", "image": "ui/icons/coin.png", "size": [50, 50]},
             {"type": "text", "name": "Coin Count", "text": "1240", "style": "coins", "fit": "both"}]},
        {"type": "panel", "name": "Hotbar", "anchor": "bottom", "position": [0, -28], "style": "cozy", "layout": "row", "gap": 8,
         "padding": [10, 12], "fit": "both", "children": slots},
        {"type": "text", "name": "Hint", "anchor": "top_left", "position": [44, 118], "style": "hint", "fit": "both",
         "text": "Click soil to plant  ·  Click ripe berries to harvest  ·  Click the stand to chat"},
        {"type": "text", "name": "Toast", "anchor": "top", "position": [0, 40], "style": "toast", "fit": "both", "text": "+12 coins"}])
    stra.call("entity_update", entity="Toast", components={"ui": {"visible": False}})
    stra.behave("HUD", "FarmHud", "Clock, coins and toasts.", HUD)
    stra.flush("HUD")

    # Juniper: the dialogue runner (her voice comes from the berry stand)
    stra.call("ui_create", template="dialogue", canvas={"name": "Dialogue UI", "theme": "light", "styleSheet": sheet, "sortOrder": 100})
    stra.call("entity_update", entity="Dialogue Box", components={"ui": {"visible": False, "size": [0, 200], "margin": [0, 330, 150, 330],
                                                                         "padding": [20, 28, 20, 20]}})
    stra.call("entity_update", entity="Dialogue Portrait", components={"ui": {"size": [160, 160]}})
    stra.op("entity_create", name="Juniper", position=[STALL[0], -0.06 * STALL[1] + 1.2, STALL[1]],
            components={"dialogue": {"script": "story/juniper.dialogue", "startNode": "Start", "typewriter": 45}})
    stra.flush("Juniper")
    stra.behave("Juniper", "Story", "Juniper's gift lands in the hotbar.", STORY)
    stra.flush("Juniper's story hooks")


# Golden hour: a low sun in the south-west, warm haze, soft shadows, drifting clouds.
MOODS = {
    "golden": dict(
        skyMode="atmosphere", sunElevation=8.5, sunAzimuth=292, sunIntensity=3.4, sunColor="#ffcf96", clouds=0.55,
        cloudHeight=1300, cloudThickness=1100, cloudScale=1.3, cloudDensity=0.8, cloudSpeed=8,
        fogDensity=0.0011, fogHeight=0.05, fogColor="#f2d0ac", haze=0.0005, godRays=0.9,
        ambient=0.72, autoExposure=True, exposureCompensation=0.25, tonemap="agx", look="golden_hour", lookStrength=0.35,
        saturation=1.16, contrast=1.04, bloomIntensity=0.45, vignette=0.18, grain=0.015, showGrid=False, windSpeed=2.5,
        windDirection=250, gi=1, ssr=1, ao=1.0, shadowDistance=260, shadowSoftness=1.6, temperature=0.08),
}


# =============================================================================================
# Hero shots: each is a Catmull-Rom camera path looking at a fixed target, played by its own
# sequence (cinematics/<name>.sequence.json); the still is taken at the middle of the move.
# A point is [x, y, z] absolute or [x, dy, z, "g"] = dy above the ground (analytic height).
# =============================================================================================
def _row_z(j):
    return FIELD[1] - (ROWS - 1) * ROW_GAP / 2 + j * ROW_GAP


SHOTS = [
    dict(name="establishing", seconds=8, fov=46,
         path=[[40, 10.0, 14], [37, 9.4, 11], [34, 8.8, 8]], target=[15, 5.0, -26]),
    dict(name="berry_rows", seconds=7, fov=36, aperture=1.8, focus=1.9,
         path=[[FIELD[0] + 8.3, 0.36, _row_z(4) + 0.55, "f"], [FIELD[0] + 7.4, 0.35, _row_z(4) + 0.55, "f"],
               [FIELD[0] + 6.5, 0.34, _row_z(4) + 0.55, "f"]], target=[FIELD[0] - 9, 0.25, _row_z(4) + 0.05]),
    dict(name="cottage_porch", seconds=7, fov=44, aperture=4.0, focus=9.5,
         path=[[HOUSE[0] + 4.2, 1.75, HOUSE[1] + 15.0, "g"], [HOUSE[0] + 3.6, 1.75, HOUSE[1] + 13.6, "g"],
               [HOUSE[0] + 3.0, 1.75, HOUSE[1] + 12.2, "g"]], target=[HOUSE[0] + 0.6, 1.9, HOUSE[1] + 3.4]),
    dict(name="greenhouse", seconds=6, fov=46,
         path=[[GREENHOUSE[0] + 9.0, 1.3, GREENHOUSE[1] + 1.5, "g"], [GREENHOUSE[0] + 8.6, 1.4, GREENHOUSE[1] + 3.0, "g"],
               [GREENHOUSE[0] + 8.0, 1.5, GREENHOUSE[1] + 4.5, "g"]], target=[GREENHOUSE[0], 1.8, GREENHOUSE[1] - 0.5]),
    dict(name="lily_pond", seconds=7, fov=44, aperture=3.2, focus=4.5,
         path=[[POND[0] + 7.6, 0.55, POND[1] + 3.2, "w"], [POND[0] + 7.0, 0.5, POND[1] + 2.2, "w"],
               [POND[0] + 6.4, 0.48, POND[1] + 1.2, "w"]], target=[POND[0] - 9.0, 1.3, POND[1] + 1.0]),
    dict(name="berry_stand", seconds=6, fov=42, aperture=3.5, focus=5.2,
         path=[[STALL[0] - 4.4, 2.3, STALL[1] + 5.2, "g"], [STALL[0] - 3.9, 2.25, STALL[1] + 4.6, "g"],
               [STALL[0] - 3.4, 2.2, STALL[1] + 4.0, "g"]], target=[STALL[0] + 0.2, 0.9, STALL[1] - 0.2]),
    dict(name="windmill", seconds=8, fov=46,
         path=[[MILL[0] - 11.0, 1.0, MILL[1] + 17.0, "g"], [MILL[0] - 10.0, 2.0, MILL[1] + 15.8, "g"],
               [MILL[0] - 9.0, 3.0, MILL[1] + 14.6, "g"]], target=[MILL[0] + 0.5, H_MILL_TOP + 8.0, MILL[1]]),
    dict(name="farm_day", seconds=6, fov=48, hud=True, dialogue=True,
         path=[[6.0, 6.6, 6.5], [5.2, 6.3, 5.6], [4.4, 6.0, 4.7]], target=[-5.5, 0.6, -9.0]),
    dict(name="miniature", seconds=8, fov=26, tilt=0.6,
         path=[[-44, 36, 46], [-40, 35, 49], [-36, 34, 52]], target=[-2, 0.5, -12]),
]
SEQ_ENV = ("sunElevation", "sunAzimuth", "sunIntensity", "sunColor", "ambient", "clouds", "cloudHeight", "cloudThickness",
           "cloudDensity", "fogColor", "fogDensity", "fogHeight", "godRays", "haze", "exposureCompensation", "look", "lookStrength",
           "saturation", "contrast", "bloomIntensity", "vignette", "shadowSoftness", "windSpeed", "temperature")


def _resolve(shot):
    pts = []
    for p in shot["path"]:
        if len(p) == 4:
            base = {"g": lambda x, z: H(x, z), "w": lambda x, z: pond_level(), "f": lambda x, z: -0.06 * z + 0.15}[p[3]](p[0], p[2])
            pts.append([round(p[0], 3), round(base + p[1], 3), round(p[2], 3)])
        else:
            pts.append([round(v, 3) for v in p])
    return pts, [round(v, 3) for v in shot["target"]]


def _catmull(pts, s):
    s = min(max(s, 0.0), 1.0)
    if len(pts) == 1:
        return list(pts[0])
    x = s * (len(pts) - 1)
    i = min(int(x), len(pts) - 2)
    u = x - i
    p0, p1, p2, p3 = pts[max(i - 1, 0)], pts[i], pts[i + 1], pts[min(i + 2, len(pts) - 1)]
    return [0.5 * (2 * b + (c - a) * u + (2 * a - 5 * b + 4 * c - d) * u * u + (3 * b - a - 3 * c + d) * u ** 3)
            for a, b, c, d in zip(p0, p1, p2, p3)]


def shot_view(shot, s=0.5):
    """Camera arguments (viewport_capture) of a hero shot at normalized time s (smooth ease, like the sequence)."""
    pts, target = _resolve(shot)
    e = s * s * (3 - 2 * s)
    v = dict(eye=[round(c, 4) for c in _catmull(pts, e)], target=target, fov=shot["fov"])
    if shot.get("aperture"):
        v.update(aperture=shot["aperture"], focus_distance=shot["focus"] or round(math.dist(v["eye"], target), 2))
    if shot.get("tilt"):
        v.update(tilt_shift=shot["tilt"], focus_distance=round(math.dist(v["eye"], target), 2))
    return v


def sequences(nim):
    """One cinematic sequence per hero shot: the mood (environment keys at t=0), HUD on/off and a path camera."""
    env = MOODS["golden"]
    for shot in SHOTS:
        pts, target = _resolve(shot)
        path = f"cinematics/{shot['name']}.sequence.json"
        tracks = [{"type": "property", "property": f"environment.{k}", "keys": [{"t": 0, "value": env[k], "ease": "step"}]}
                  for k in SEQ_ENV if k in env]
        nim.call("sequence_create", path=path, name=f"Berrybrook — {shot['name'].replace('_', ' ')}", duration=shot["seconds"],
                 tracks=tracks, entity=f"Sequence {shot['name']}", play_on_start=False, overwrite=True)
        cam = f"Cam {shot['name']}"
        nim.call("sequence_camera_shot", sequence=path, camera=cam, shot="path", start=0, duration=shot["seconds"], points=pts,
                 target=target, fov=shot["fov"], ease="smooth")
        lens = {"farPlane": 6000, "nearPlane": 0.05, "aperture": shot.get("aperture", 0), "primary": False}
        if shot.get("aperture"):
            lens["focusDistance"] = shot["focus"] or round(math.dist(pts[len(pts) // 2], target), 2)
        if shot.get("tilt"):
            lens.update(tiltShift=shot["tilt"], focusDistance=round(math.dist(pts[len(pts) // 2], target), 2))
        nim.call("entity_update", entity=cam, components={"camera": lens})
        events = [{"t": 0.0, "event": "hud_show" if shot.get("hud") else "hud_hide", "target": "HUD"}]
        nim.call("sequence_key", sequence=path, events=events)


def _prepare(sky, shot):
    """Scene state for a shot: HUD shown only in gameplay shots; Juniper mid-conversation in farm_day."""
    sky.call("sim_input", event="hud_show" if shot.get("hud") else "hud_hide", target="HUD")
    if shot.get("dialogue"):
        sky.call("dialogue_control", action="start", entity="Juniper")
    else:
        sky.call("dialogue_control", action="stop", entity="Juniper")


def shots():
    """showcase.py render hooks: every hero shot as its camera move."""
    out = []
    for shot in SHOTS:
        def cam(i, n, shot=shot):
            return shot_view(shot, i / max(n - 1, 1))

        def before(sky, i, n, shot=shot):
            if i == 0:
                _prepare(sky, shot)
        out.append(dict(name=shot["name"], frames=int(shot["seconds"] * 30), cam=cam, before=before, warmup=150))
    return out


def render_stills(out_dir="shots", width=1920, height=1080, samples=16, only=None, scene="scenes/main.sky.json"):
    """Final stills (middle of each move), one capture at a time, saved as JPEG quality 90."""
    import subprocess
    from sky import Sky
    sky = Sky(project=PROJECT, scene=scene)
    sky.call("camera_set", eye=[0, 0, 40], target=[0, 0, 0])
    os.makedirs(os.path.join(PROJECT, out_dir), exist_ok=True)
    sky.call("sim_control", action="play")
    sky.call("sim_control", action="step", ticks=240)
    for shot in SHOTS:
        if only and shot["name"] not in only:
            continue
        _prepare(sky, shot)
        sky.call("sim_control", action="step", ticks=150 if shot.get("dialogue") else 2)
        args = dict(width=width, height=height, annotate=False, overlays=False, include_image=False, samples=samples,
                    save_path=f"{out_dir}/{shot['name']}.png", **shot_view(shot))
        sky.call("viewport_capture", **args)
        png = os.path.join(PROJECT, out_dir, shot["name"] + ".png")
        jpg = png[:-4] + ".jpg"
        q = 90
        while True:
            subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", str(q), png, "--out", jpg], check=True,
                           capture_output=True)
            if os.path.getsize(jpg) < 1.45 * 1024 * 1024 or q <= 70:
                break
            q -= 4
        os.remove(png)
        print(shot["name"], os.path.getsize(jpg) // 1024, "KB", f"q{q}", flush=True)
    sky.close()


def contact_sheet(out_dir="shots", cols=3, cell=(640, 360), gutter=8):
    from PIL import Image
    names = [s["name"] for s in SHOTS if os.path.exists(os.path.join(PROJECT, out_dir, s["name"] + ".jpg"))]
    rows = (len(names) + cols - 1) // cols
    W, Hh = cols * cell[0] + (cols + 1) * gutter, rows * cell[1] + (rows + 1) * gutter
    sheet = Image.new("RGB", (W, Hh), (28, 22, 20))
    for i, n in enumerate(names):
        im = Image.open(os.path.join(PROJECT, out_dir, n + ".jpg")).convert("RGB").resize(cell, Image.LANCZOS)
        sheet.paste(im, (gutter + (i % cols) * (cell[0] + gutter), gutter + (i // cols) * (cell[1] + gutter)))
    path = os.path.join(PROJECT, out_dir, "contact_sheet.jpg")
    sheet.save(path, quality=88)
    print("contact sheet", os.path.getsize(path) // 1024, "KB")


def previews(names=("berry_rows", "cottage_porch", "windmill", "lily_pond"), seconds=4.0):
    """720p HEVC previews of a few hero moves (movie_render, 4 s, 8 samples)."""
    from sky import Sky
    sky = Sky(project=PROJECT, scene="scenes/main.sky.json")
    os.makedirs(os.path.join(PROJECT, "previews"), exist_ok=True)
    for n in names:
        shot = next(s for s in SHOTS if s["name"] == n)
        t0 = max(0.0, (shot["seconds"] - seconds) / 2)
        r = sky.call("movie_render", sequence=f"cinematics/{n}.sequence.json", start=t0, duration=seconds, resolution="720p", fps=30,
                     samples=8, codec="hevc", output=f"previews/{n}.mp4", simulate=True)
        print(n, r.get("frames") if isinstance(r, dict) else r, flush=True)
    sky.close()


def reshoot(studio):
    """Re-derive the hero-shot sequences from the saved world (fast iteration on cameras)."""
    nim = studio.agent("Nimbus")
    nim.call("scene_load", path="scenes/main.sky.json")
    sequences(nim)
    nim.call("scene_save", path="scenes/main.sky.json")


if __name__ == "__main__":
    os.makedirs(PROJECT, exist_ok=True)
    args = sys.argv[1:]
    if args and args[0] == "stills":
        render_stills(only=args[1:] or None)
        contact_sheet()
        sys.exit(0)
    if args and args[0] == "sheet":
        contact_sheet()
        sys.exit(0)
    if args and args[0] == "previews":
        previews(*([tuple(args[1:])] if args[1:] else []))
        sys.exit(0)
    st = Studio(PROJECT)
    try:
        if "--lineup" in args:
            lineup(st)
        elif "--shots" in args:
            reshoot(st)
        else:
            build(st)
    finally:
        st.close()
    meta = {k: v for k, v in META.items() if k != "view"}
    with open(os.path.join(PROJECT, "game.json"), "w") as f:
        json.dump(meta, f, indent=2)
