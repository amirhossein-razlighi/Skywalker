"""Tidebreak Isle — a tropical-island open-world adventure (original IP; benchmark: the best
seafaring AAA games).

A sculpted volcanic island: a crescent cove of photoscanned sand that darkens where the
waves run up, crystal shallows over a sand shelf, dunes of wind-blown grass, island trees
and shrubs, a headland of sea cliffs crowned by a ruined watchtower, a fishing jetty, a
merchant brig riding at anchor, a driftwood campfire at dusk and gulls wheeling overhead.
"""
import math
import os

import polyhaven as ph
from kit import rnd

META = dict(
    id="tidebreak_isle", title="Tidebreak Isle", genre="Open-world Island Adventure", mood="golden hour",
    pitch="The storm left you one ship, one map and a cove nobody charts. Make landfall before dark.",
    view=dict(eye=[-34, 6, 150], target=[20, 2, 230]),
    assets="Poly Haven (CC0), Blender procedural (Skywalker DCC)",
)

SEA = 0.0                  # water level (world y)
BAY = (0.0, 262.0)         # center of the cove's circular bay
SHORE_R = 66.0             # bay radius at the waterline
HEAD = (104.0, 238.0)      # headland knoll (watchtower), on the spine
HEAD_HALF = 18.0           # half-width of the headland plateau (where the cliff faces stand)
CLIFF_SCALE = 1.25         # the coastal_cliff_04 scans, scaled to a 12.9 m face
CLIFF_BASE = -1.2          # their base, under the waterline
HEAD_TOP = 16.0            # height of the headland ridge
CAMP = (-22.0, 0.0)        # campfire (z is solved on the beach profile)
SHIP = (34.0, 330.0)       # brig at anchor

# Two lighting moods (environment_update arguments).
GOLDEN = dict(skyMode="atmosphere", sunElevation=7.5, sunAzimuth=12, sunIntensity=3.4, sunColor="#ffd0a0",
              clouds=0.55, cloudMode="volumetric", cloudHeight=1400, cloudThickness=1600, cloudScale=1.2, cloudDensity=1.0,
              cloudSpeed=6, ambient=0.55, reflections=1.0, fogColor="#cdb9a6", fogDensity=0.0006, fogHeight=0.04,
              godRays=0.9, haze=0.0025, gi=0.8, ssr=1.0, taa=True, ao=1.0, aoRadius=0.8, shadowSoftness=1.1,
              tonemap="agx", autoExposure=True, exposureCompensation=-0.1, look="golden_hour", lookStrength=0.18,
              saturation=1.0, contrast=1.08, bloomIntensity=0.3, bloomThreshold=1.4, vignette=0.22, grain=0.04,
              windSpeed=5, windDirection=200, showGrid=False)
MIDDAY = dict(skyMode="atmosphere", sunElevation=58, sunAzimuth=150, sunIntensity=3.0, sunColor="#fff4e6",
              clouds=0.32, cloudMode="volumetric", cloudHeight=1500, cloudThickness=1200, cloudScale=1.1, cloudDensity=0.8,
              cloudSpeed=8, ambient=0.6, reflections=1.0, fogColor="#bcd6ee", fogDensity=0.0005, fogHeight=0.03,
              godRays=0.25, haze=0.0012, gi=0.8, ssr=1.0, taa=True, ao=1.0, aoRadius=0.8, shadowSoftness=1.4,
              tonemap="agx", autoExposure=True, exposureCompensation=0.0, look="vivid", lookStrength=0.2,
              saturation=1.1, contrast=1.06, bloomIntensity=0.2, bloomThreshold=1.6, vignette=0.15, grain=0.03,
              windSpeed=6, windDirection=200, showGrid=False)


def _bay_point(r, deg):
    """Point at distance r from the bay center; deg 90 = straight inland (toward -z)."""
    a = math.radians(deg)
    return BAY[0] + r * math.cos(a), BAY[1] - r * math.sin(a)


HEAD_SPINE = [(82.0, 168.0), (103.0, 234.0), (126.0, 294.0)]  # the headland bends once on its way out to sea


def _head_axis(t):
    """The headland's spine, from where it leaves the island (t=0) to its seaward tip (t=1)."""
    (x0, z0), (x1, z1), (x2, z2) = HEAD_SPINE
    if t < 0.5:
        u = t * 2
        return x0 + (x1 - x0) * u, z0 + (z1 - z0) * u
    u = (t - 0.5) * 2
    return x1 + (x2 - x1) * u, z1 + (z2 - z1) * u


def _scan_origin(face_center, face_dir, face_z, center_x, scale):
    """Where to put a cliff scan (its face along local +Z at z=face_z, centered at x=center_x)
    so that its face is centered on `face_center` and looks along `face_dir`."""
    fx, fz = face_dir
    yaw = math.atan2(fx, fz)
    c, s = math.cos(yaw), math.sin(yaw)
    lx, lz = center_x * scale, face_z * scale  # local offset of the face center, rotated: (x c + z s, -x s + z c)
    return face_center[0] - (lx * c + lz * s), face_center[1] - (-lx * s + lz * c)


def beach_height(d):
    """Designed cross-section of the cove: d = meters inland from the waterline (negative = sea)."""
    pts = [(-90, -9.0), (-60, -5.5), (-40, -3.2), (-24, -1.7), (-12, -0.8), (-4, -0.25), (0, 0.05), (8, 0.45),
           (18, 0.95), (30, 1.6), (40, 2.2), (48, 3.2), (56, 4.4), (64, 4.8), (80, 7.0), (100, 12.0)]
    if d <= pts[0][0]:
        return pts[0][1]
    for (d0, h0), (d1, h1) in zip(pts, pts[1:]):
        if d <= d1:
            t = (d - d0) / (d1 - d0)
            t = t * t * (3 - 2 * t)
            return h0 + (h1 - h0) * t
    return pts[-1][1]


def sculpt_island(cir):
    """The cove, its beach profile, the dunes, the eastern headland and the western reef point."""
    strokes = []
    # 1. Scoop the bay out of the island's south shore, then lay the designed beach profile
    #    on concentric arcs (outer first) so the cove is a clean, gently shelving crescent.
    for r in range(int(SHORE_R) + 98, 0, -6):
        d = r - SHORE_R
        lo, hi = (35, 145) if d > 56 else (14, 166) if d > -8 else (-12, 192)
        step = max(3.0, 360.0 * 5.0 / (2 * math.pi * max(r, 6)))  # ~5 m between strokes along the arc
        deg = lo
        while deg <= hi:
            x, z = _bay_point(r, deg)
            strokes.append(dict(x=x, z=z, radius=11, strength=0.9 if d < 56 else 0.35, mode="flatten", target=beach_height(d),
                                falloff=0.7))
            deg += step
    # 2. Dunes: a broken ridge behind the dry sand.
    r = rnd(5150)
    for row, (dd, step) in enumerate(((47, 9), (56, 11))):
        deg = 24 + row * 5
        while deg < 156:
            x, z = _bay_point(SHORE_R + dd + r.uniform(-3, 3), deg + r.uniform(-2, 2))
            strokes.append(dict(x=x, z=z, radius=r.uniform(13, 20), strength=r.uniform(0.6, 1.5), mode="raise", falloff=1.0))
            deg += step * r.uniform(0.8, 1.25)
    # 3. The eastern headland: a crooked ridge running out to sea, its sides cut sheer where
    #    the swell has eaten into it (the sea cliffs), crags on top, a knoll for the watchtower.
    hr = rnd(77)
    for k in range(30):  # first carve the sea in around it, so the cliffs drop into deep water
        t = 0.2 + 0.8 * k / 29
        x, z = _head_axis(t)
        x2, z2 = _head_axis(min(1.0, t + 0.02))
        dx, dz = x2 - x, z2 - z
        n = math.hypot(dx, dz) or 1
        for side in (1, -1):
            off = HEAD_HALF + hr.uniform(10, 22)
            px, pz = x - dz / n * off * side, z + dx / n * off * side
            depth = -2.2 if (side > 0 and t < 0.5) else hr.uniform(-7, -4)
            strokes.append(dict(x=px, z=pz, radius=hr.uniform(10, 16), strength=0.8, mode="flatten", target=depth, falloff=0.8))
    for k in range(34):
        t = k / 33
        x, z = _head_axis(t)
        x2, z2 = _head_axis(min(1.0, t + 0.02))
        dx, dz = x2 - x, z2 - z
        n = math.hypot(dx, dz) or 1
        j = hr.uniform(-5, 5)
        strokes.append(dict(x=x - dz / n * j, z=z + dx / n * j, radius=hr.uniform(12, 19) * (1 - 0.25 * t), strength=1.0,
                            mode="flatten", target=HEAD_TOP - 3.0 * t * t + hr.uniform(-1.2, 1.2), falloff=0.5))
    for k in range(12):
        x, z = _head_axis(0.1 + k * 0.075)
        strokes.append(dict(x=x, z=z, radius=hr.uniform(8, 13), strength=1.2, mode="noise", falloff=0.9))
    for k in range(18):  # buttresses and gullies on the cliff faces
        t = 0.2 + 0.8 * k / 17
        x, z = _head_axis(t)
        x2, z2 = _head_axis(min(1.0, t + 0.02))
        dx, dz = x2 - x, z2 - z
        n = math.hypot(dx, dz) or 1
        for side in (1, -1):
            off = HEAD_HALF - hr.uniform(3, 7)
            strokes.append(dict(x=x - dz / n * off * side, z=z + dx / n * off * side, radius=hr.uniform(6, 10),
                                strength=hr.uniform(2.0, 3.5), mode="noise", falloff=0.85))
    strokes.append(dict(x=HEAD[0], z=HEAD[1], radius=9, strength=0.8, mode="flatten", target=HEAD_TOP + 0.6, falloff=0.7))
    # 4. The western reef point: low rocky spit.
    for k in range(7):
        t = k / 6
        x, z = -82 - 22 * t, 205 + 55 * t
        strokes.append(dict(x=x, z=z, radius=20 - 7 * t, strength=0.8, mode="flatten", target=4.5 - 4.8 * t, falloff=0.6))
    # 5. Natural roughness, then a light smoothing pass over the beach.
    for k in range(10):
        x, z = _bay_point(SHORE_R + 30, 18 + k * 16)
        strokes.append(dict(x=x, z=z, radius=22, strength=0.25, mode="noise", falloff=0.8))
    cir.call("terrain_sculpt", entity="Island", strokes=strokes)
    smooth = []
    for deg in range(10, 175, 6):
        for d in (-20, -6, 8, 22, 44, 52, 60):
            x, z = _bay_point(SHORE_R + d, deg)
            smooth.append(dict(x=x, z=z, radius=10, strength=0.8, mode="smooth", falloff=0.8))
    cir.call("terrain_sculpt", entity="Island", strokes=smooth)


MODELS = ("island_tree_01", "island_tree_02", "island_tree_03", "dutch_ship_large_02", "ship_pinnace", "modular_fort_01",
          "modular_wooden_pier", "coastal_cliff_01", "coastal_cliff_02", "coastal_cliff_04", "coast_line_01", "coast_line_02",
          "coast_rocks_01", "coast_rocks_02", "coast_rocks_03", "coast_rocks_05", "coast_land_rocks_02", "coast_land_rocks_03",
          "coast_land_rocks_04", "sand_rocks_small_01", "dead_tree_trunk", "dead_tree_trunk_02", "root_cluster_01", "lambis_shell",
          "pachira_aquatica_01", "calathea_orbifolia_01", "anthurium_botany_01", "fern_02", "grass_bermuda_01", "shrub_02",
          "shrub_01", "stone_fire_pit", "wooden_barrels_01", "wooden_crate_01", "wooden_bucket_01", "wooden_lantern_01",
          "lifebuoy", "ocean_buoy", "lateral_sea_marker", "treasure_chest", "boulder_01")
FORT_MODULES = (12, 1, 7, 2, 15, 4)  # the round tower, two curtain walls, corner pieces


def assets(pix):
    """Poly Haven downloads (CC0) plus the Blender-made palms, gull and fort pieces."""
    import tidebreak_blender as TB
    A = {k: ph.model(pix, k, res="2k") for k in MODELS}

    def blender(script, out, files, args=()):
        missing = [f for f in files if not os.path.exists(os.path.join(pix.studio.project, out, f))]
        if missing:
            pix.call("dcc_run_script", script=getattr(TB, script), name=script.lower(), out_dir=out, args=[str(a) for a in args],
                     timeout_s=1200)
        for f in files:
            A[f.rsplit(".", 1)[0]] = pix.call("asset_import", path=f"{out}/{f}", normalize=False)

    blender("PALMS", "downloads/dcc/palms", [f"palm_{v}.glb" for v in "abcde"])
    blender("GULL", "downloads/dcc/gull", ["gull_body.glb", "gull_wing_l.glb", "gull_wing_r.glb"])
    blender("SPLIT", "downloads/dcc/fort", [f"fort_module_{m}.glb" for m in FORT_MODULES],
            ["downloads/modular_fort_01/modular_fort_01_2k.gltf", "fort_module", *FORT_MODULES])
    blender("SPLIT", "downloads/dcc/pier", ["pier_0.glb"], ["downloads/modular_wooden_pier/modular_wooden_pier_2k.gltf", "pier", 0])
    return A


class World:
    """Ground heights (terrain_query, cached) and the named spots the shots frame."""

    def __init__(self, b):
        self.b, self.cache, self.spots, self.shots = b, {}, {}, {}

    def y(self, x, z):
        key = (round(x, 2), round(z, 2))
        if key not in self.cache:
            p = self.b.call("terrain_query", points=[[x, z]])["points"][0]
            self.cache[key] = p.get("height", 0.0)
        return self.cache[key]

    def spot(self, name, x, z, dy=0.0):
        self.spots[name] = [round(x, 3), round(self.y(x, z) + dy, 3), round(z, 3)]
        return self.spots[name]

    def save(self, project):
        import json
        with open(os.path.join(project, "shot_heights.json"), "w") as f:
            json.dump({"spots": self.spots, "shots": self.shots}, f, indent=1)


def _layer(res, **fields):
    layer = {"prefab": res["prefab"]} if res.get("prefab") else {"mesh": res["mesh"], "material": res.get("material", "")}
    layer.update(fields)
    return layer


def vegetation(cir, A):
    """GPU-instanced foliage: dune grass and wrack line on the beach, palms on the dunes, island
    trees and a tropical understory on the slopes."""
    palms = [_layer(A[k], name=k, density=0.003, scaleMin=0.8, scaleMax=1.25, heightMin=2.6, heightMax=60, slopeMax=38,
                    clumping=0.75, cullDistance=1600, wind=0.45, randomTilt=5, subsurface=0.45, seed=31 + i)
             for i, k in enumerate(("palm_a", "palm_b", "palm_c", "palm_d", "palm_e"))]
    trees = [_layer(A[k], name=k, density=0.00018, scaleMin=1.6, scaleMax=2.4, heightMin=8, slopeMax=36,
                    clumping=0.8, cullDistance=1400, wind=0.25, randomTilt=4, subsurface=0.45, seed=41 + i)
             for i, k in enumerate(("island_tree_01", "island_tree_02", "island_tree_03"))]
    cir.call("foliage_add", entity="Island", name="Island Foliage", seed=7, layers=[
        {"preset": "dune_grass", "density": 4.5, "heightMin": 2.3, "heightMax": 12, "slopeMax": 30, "scaleMin": 0.85,
         "scaleMax": 1.35, "color": "#b4a865", "clumping": 0.8},
        {"preset": "tall_grass", "density": 1.2, "heightMin": 4.5, "slopeMax": 34, "terrainLayer": 4, "color": "#8a9a48",
         "clumping": 0.75, "seed": 3},
        {"preset": "tall_grass", "name": "jungle grass", "density": 2.5, "heightMin": 6.5, "slopeMax": 44, "terrainLayer": 5,
         "color": "#4e7a2a", "clumping": 0.5, "cullDistance": 110, "seed": 4},
        {"preset": "ferns", "name": "jungle ferns", "density": 0.45, "heightMin": 6, "slopeMax": 46, "color": "#3d6b22",
         "clumping": 0.7, "cullDistance": 120, "seed": 6},
        _layer(A["grass_bermuda_01"], name="bermuda", density=3.0, scaleMin=0.8, scaleMax=1.4, heightMin=6, slopeMax=36,
               terrainLayer=5, clumping=0.6, cullDistance=70, wind=0.9, castShadows=False, subsurface=0.55),
        {"preset": "shells", "density": 0.3, "heightMin": 0.9, "heightMax": 1.6, "scaleMin": 0.45, "scaleMax": 0.9, "clumping": 0.85},
        {"preset": "beach_pebbles", "density": 0.08, "heightMin": -0.8, "heightMax": 0.3, "scaleMin": 0.4, "scaleMax": 1.0, "clumping": 0.9, "seed": 5},
        _layer(A["sand_rocks_small_01"], name="tide stones", density=0.006, scaleMin=0.6, scaleMax=1.3, terrainLayer=3,
               clumping=0.7, cullDistance=260, alignToNormal=0.8, sink=0.08, wind=0),
        *palms, *trees,
        _layer(A["shrub_02"], name="shrubs", density=0.025, scaleMin=0.8, scaleMax=1.6, heightMin=4.2, slopeMax=44, clumping=0.75,
               cullDistance=300, wind=0.6, subsurface=0.5, seed=51),
        _layer(A["pachira_aquatica_01"], name="pachira", density=0.012, scaleMin=0.9, scaleMax=1.6, heightMin=5, slopeMax=42,
               clumping=0.8, cullDistance=260, wind=0.5, subsurface=0.5, seed=52),
        _layer(A["calathea_orbifolia_01"], name="calathea", density=0.03, scaleMin=0.9, scaleMax=1.6, heightMin=5.5,
               slopeMax=36, terrainLayer=5, clumping=0.85, cullDistance=120, wind=0.6, castShadows=False, subsurface=0.5, seed=53),
        _layer(A["anthurium_botany_01"], name="anthurium", density=0.02, scaleMin=0.9, scaleMax=1.5, heightMin=4.8,
               slopeMax=36, clumping=0.85, cullDistance=120, wind=0.6, castShadows=False, subsurface=0.5, seed=54),
        _layer(A["fern_02"], name="ferns", density=0.09, scaleMin=1.0, scaleMax=1.9, heightMin=6, slopeMax=46,
               clumping=0.8, cullDistance=90, wind=0.7, castShadows=False, subsurface=0.5, seed=55),
    ])


def _yaw_to(vx, vz):
    """Yaw (degrees) that turns a model's +X axis toward the horizontal direction (vx, vz)."""
    return math.degrees(math.atan2(-vz, vx))


def rocks(cir, A, world):
    """Photoscanned sea cliffs under the headland, reef shelves at the western point, surf rocks."""
    r = rnd(808)
    # Fallen rock at the foot of the headland cliffs: walk out from the spine until the ground
    # meets the sea, and pile photoscanned boulders there (the surf foams around them).
    kinds = ("coast_land_rocks_02", "coast_land_rocks_03", "coast_land_rocks_04", "coast_rocks_05", "boulder_01")
    k = 0
    for i in range(22):
        t = 0.22 + 0.78 * i / 21
        x, z = _head_axis(t)
        x2, z2 = _head_axis(min(1.0, t + 0.02))
        dx, dz = x2 - x, z2 - z
        n = math.hypot(dx, dz) or 1
        for side in (1, -1):
            if r.random() < 0.1:
                continue
            for off in range(6, 46, 2):
                px, pz = x - dz / n * off * side, z + dx / n * off * side
                if world.y(px, pz) < 0.8:
                    break
            off += r.uniform(-1, 2)
            px, pz = x - dz / n * off * side, z + dx / n * off * side
            kind = kinds[r.randrange(len(kinds))]
            sc = r.uniform(1.6, 3.2) if kind in ("coast_rocks_05", "boulder_01") else r.uniform(1.0, 1.8)
            k += 1
            ph.place(cir, A[kind], f"Cliff Foot Rock {k}", (px, min(world.y(px, pz), 0.3) - 0.35 * sc, pz), yaw=r.uniform(0, 360),
                     rot=(r.uniform(-12, 12), r.uniform(0, 360), r.uniform(-12, 12)), scale=sc, tags=["rock"])
    (ax1, az1), (ax0, az0) = _head_axis(1.0), _head_axis(0.95)
    dx, dz = ax1 - ax0, az1 - az0
    n = math.hypot(dx, dz)
    dx, dz = dx / n, dz / n
    ph.place(cir, A["coastal_cliff_02"], "Sea Stack", (ax1 + dx * 26 + dz * 8, -1.4, az1 + dz * 26 - dx * 8),
             yaw=math.degrees(math.atan2(dx, dz)) + 35, scale=0.9, tags=["cliff"])
    # Rock shelves that the surf breaks over.
    ph.place(cir, A["coast_rocks_01"], "Headland Reef", (112, -1.1, 312), yaw=20, scale=1.0, tags=["rock"])
    ph.place(cir, A["coast_rocks_02"], "Cliff Foot Rocks", (66, -0.9, 262), yaw=200, scale=0.8, tags=["rock"])
    ph.place(cir, A["coast_line_01"], "West Reef", (-102, -0.7, 250), yaw=35, scale=1.0, tags=["rock"])
    ph.place(cir, A["coast_line_02"], "West Reef Shelf", (-86, -0.6, 222), yaw=-60, scale=0.9, tags=["rock"])
    ph.place(cir, A["coast_rocks_03"], "Reef Point Rocks", (-118, -0.8, 282), yaw=70, scale=1.1, tags=["rock"])
    cir.flush("Sea cliffs and reefs")
    # Boulder clusters where the beach meets the headland and the reef (sunk into the sand).
    clusters = [(62, 236, "coast_land_rocks_03", 1.3, 30), (57, 222, "coast_land_rocks_02", 1.2, 80), (66, 250, "coast_rocks_05", 1.6, 0),
                (-60, 226, "coast_land_rocks_04", 1.2, 140), (-70, 214, "coast_land_rocks_03", 1.1, 200),
                (-64, 236, "coast_rocks_05", 1.4, 50)]
    for k, (x, z, kind, sc, yaw) in enumerate(clusters):
        ph.place(cir, A[kind], f"Beach Rocks {k + 1}", (x, world.y(x, z) - 0.25 * sc, z), yaw=yaw, scale=sc, tags=["rock"])
    for k in range(14):
        deg = r.uniform(10, 30) if k % 2 else r.uniform(150, 170)
        x, z = _bay_point(SHORE_R + r.uniform(-14, 4), deg)
        sc = r.uniform(0.9, 2.2)
        ph.place(cir, A["boulder_01" if k % 3 else "coast_rocks_05"], f"Surf Rock {k + 1}", (x, world.y(x, z) - 0.3 * sc, z),
                 yaw=r.uniform(0, 360), scale=sc, tags=["rock"])
    cir.flush("Boulders where the sand ends")


def landmarks(pix, A, world):
    """The watchtower on the headland, the fishing jetty, the brig at anchor and channel markers."""
    hx, hz = HEAD
    hy = world.y(hx, hz)
    world.spot("tower", hx, hz)
    ph.place(pix, A["fort_module_12"], "Tidebreak Watchtower", (hx, hy - 0.6, hz), yaw=20, tags=["landmark"])
    ph.place(pix, A["fort_module_7"], "Broken Rampart", (hx - 9, world.y(hx - 9, hz - 14) - 2.2, hz - 14), yaw=-62, scale=0.8,
             tags=["ruin"])
    ph.place(pix, A["fort_module_4"], "Rampart Corner", (hx - 13, world.y(hx - 13, hz - 25) - 1.8, hz - 25), yaw=-40, scale=0.8,
             tags=["ruin"])
    ph.place(pix, A["fort_module_2"], "Fallen Gate", (hx + 3, world.y(hx + 3, hz + 16) - 3.0, hz + 16), yaw=110, scale=0.75,
             tags=["ruin"])
    top = hy - 0.6 + 13.5
    world.spots["beacon"] = [hx, round(top, 3), hz]
    pix.flush("Watchtower and ruins")
    # A beacon fire on the tower's battlements.
    pix.call("fx_create", effect="torch", name="Beacon", position=[hx, top - 0.6, hz], overrides={"lightRange": 26})

    # The jetty: five 12.4 m sections of Poly Haven's pier kit, from the western beach out into the cove.
    px, pz = _bay_point(SHORE_R + 6, 150)
    dx, dz = BAY[0] - px, BAY[1] - pz
    n = math.hypot(dx, dz)
    dx, dz = dx / n, dz / n
    yaw = math.degrees(math.atan2(dx, dz))
    for k in range(5):
        cx, cz = px + dx * (6.2 + 12.41 * k), pz + dz * (6.2 + 12.41 * k)
        ph.place(pix, A["pier_0"], f"Jetty {k + 1}", (cx, -0.94, cz), yaw=yaw + (180 if k == 4 else 0), tags=["pier"])
    world.spots["jetty_root"] = [round(px, 2), 1.5, round(pz, 2)]
    world.spots["jetty_end"] = [round(px + dx * 62, 2), 1.5, round(pz + dz * 62, 2)]
    world.spots["jetty_dir"] = [round(dx, 4), 0, round(dz, 4)]
    # The brig at anchor, and a pinnace standing off on the horizon.
    sx, sz = SHIP
    ph.place(pix, A["dutch_ship_large_02"], "Brig Marigold", (sx, 0, sz), yaw=75, tags=["ship"])
    ph.place(pix, A["ship_pinnace"], "Pinnace Far Sail", (-260, 0, 560), yaw=100, tags=["ship"])
    world.spots["ship"] = [sx, 0, sz]
    for k, (x, z, kind) in enumerate([(-18, 300, "lateral_sea_marker"), (52, 286, "ocean_buoy"), (-40, 330, "ocean_buoy")]):
        ph.place(pix, A[kind], f"Channel Marker {k + 1}", (x, 0, z), yaw=k * 40, tags=["buoy"])
    pix.flush("Jetty, brig, channel markers")


def camp(pix, A, world):
    """A castaway's camp in the lee of the dunes: driftwood fire, salvaged cargo, lanterns."""
    cx, cz = _bay_point(SHORE_R + 34, 116)
    gy = world.y(cx, cz)
    world.spot("camp", cx, cz)
    ph.place(pix, A["stone_fire_pit"], "Fire Pit", (cx, gy + 0.08, cz))
    seats = [(2.4, 0.4, 100, "dead_tree_trunk_02", 0.9), (-1.2, 2.3, 20, "dead_tree_trunk_02", 0.85),
             (-1.9, -1.8, -35, "dead_tree_trunk", 1.0)]
    for k, (ox, oz, yaw, kind, sc) in enumerate(seats):
        x, z = cx + ox, cz + oz
        ph.place(pix, A[kind], f"Driftwood Seat {k + 1}", (x, world.y(x, z) + (0.12 if kind == "dead_tree_trunk_02" else 0.0), z),
                 yaw=yaw, scale=sc, tags=["driftwood"])
    for k, (ox, oz, yaw, kind) in enumerate([(-6.2, 1.0, 25, "wooden_barrels_01"), (5.2, 1.6, -20, "wooden_crate_01"),
                                              (5.9, 2.6, 15, "wooden_crate_01"), (-3.6, 2.9, -70, "treasure_chest"),
                                              (2.7, 2.6, 0, "wooden_bucket_01")]):
        x, z = cx + ox, cz + oz
        ph.place(pix, A[kind], f"Salvage {k + 1}", (x, world.y(x, z) - 0.03, z), yaw=yaw, tags=["prop"])
    x, z = cx + 5.2, cz + 1.6
    crate_top = world.y(x, z) + 0.34
    ph.place(pix, A["wooden_lantern_01"], "Camp Lantern", (x, crate_top, z))
    pix.light("Camp Lantern Light", (x, crate_top + 0.35, z), "#ffb060", 2.4, range_=7)
    x, z = cx + 6.0, cz + 0.4
    ph.place(pix, A["lifebuoy"], "Lifebuoy", (x, world.y(x, z) + 0.35, z), rot=(-12, 200, 0))
    pix.flush("The camp")
    pix.call("fx_create", effect="campfire", name="Campfire", position=[cx, gy + 0.15, cz], overrides={"lightRange": 14})
    pix.call("fx_create", effect="fireflies", name="Dune Fireflies", position=[cx - 2, gy + 1.2, cz - 9],
             overrides={"shapeSize": [26, 2.0, 10], "rate": 6, "maxParticles": 90})
    # Hero palms leaning out over the beach, and driftwood along the high-tide line.
    r = rnd(1201)
    for k, (deg, d, kind) in enumerate([(122, 40, "palm_b"), (110, 44, "palm_a"), (128, 47, "palm_c"), (98, 41, "palm_b"),
                                         (84, 46, "palm_a"), (70, 42, "palm_b"), (60, 48, "palm_c"), (138, 42, "palm_a"),
                                         (146, 46, "palm_b"), (102, 50, "palm_c")]):
        x, z = _bay_point(SHORE_R + d, deg)
        sx, sz = BAY[0] - x, BAY[1] - z
        ph.place(pix, A[kind], f"Hero Palm {k + 1}", (x, world.y(x, z) - 0.15, z), yaw=_yaw_to(sx, sz) + r.uniform(-25, 25),
                 scale=r.uniform(0.9, 1.15), tags=["palm"])
    for k in range(9):
        x, z = _bay_point(SHORE_R + r.uniform(13, 24), r.uniform(30, 150))
        ph.place(pix, A["dead_tree_trunk" if k % 2 else "dead_tree_trunk_02"], f"Driftwood {k + 1}", (x, world.y(x, z) - 0.05, z),
                 rot=(0, r.uniform(0, 360), r.uniform(-4, 4)), scale=r.uniform(0.7, 1.2), tags=["driftwood"])
    for k in range(7):
        x, z = cx + r.uniform(-9, 9), cz + r.uniform(4, 14)
        ph.place(pix, A["lambis_shell"], f"Conch {k + 1}", (x, world.y(x, z) + 0.005, z), rot=(r.uniform(-8, 8), r.uniform(0, 360), 0),
                 scale=r.uniform(1.4, 2.2), tags=["shell"])
    ph.place(pix, A["root_cluster_01"], "Bleached Roots", (cx + 11, world.y(cx + 11, cz + 3) - 0.1, cz + 3), yaw=70, scale=1.1)
    pix.flush("Palms, driftwood and shells")


GULL_SRC = """
on tick
  let a = time * self.speed + self.phase
  let c = (self.cx, self.h + sin(time * 0.37 + self.phase) * 2.5, self.cz)
  self.position = c + (cos(a) * self.r, 0, sin(a) * self.r * 0.75)
  let dir = 1
  if self.speed < 0 then
    dir = -1
  end
  -- heading along the circle (the model faces +Z), banking into the turn
  self.rotation = (sin(time * 0.6 + self.phase) * 4, 0 - a * 57.2958 + 90 - 90 * dir, 0 - 18 * dir)
end"""
WING_SRC = """
on tick
  let g = max(0, sin(time * 0.45 + self.parent.phase * 3))
  self.rotation = (0, 0, self.side * (8 + sin(time * 9 + self.parent.phase) * 34 * g))
end"""


def life(stra, A, world):
    """Gulls wheeling over the cove, the brig riding the swell, the beacon flickering."""
    r = rnd(4242)
    for k in range(10):
        cx, cz = r.uniform(-50, 80), r.uniform(200, 320)
        name = f"Gull {k + 1}"
        speed = r.uniform(0.18, 0.32) * (1 if k % 3 else -1)
        stra.e(name, A["gull_body"]["mesh"], pos=(cx, 20, cz), scale=1.25, material=A["gull_body"].get("material"), tags=["gull"],
               vars={"cx": cx, "cz": cz, "r": r.uniform(18, 45), "h": r.uniform(14, 34), "speed": speed, "phase": r.uniform(0, 6.28)})
        for side, wing in ((1, "gull_wing_r"), (-1, "gull_wing_l")):
            stra.e(f"{name} Wing {'R' if side > 0 else 'L'}", A[wing]["mesh"], pos=(0.05 * side, 0.03, -0.02), parent=name,
                   material=A[wing].get("material"), doubleSided=True, vars={"side": side})
    stra.flush("Gulls")
    for k in range(10):
        name = f"Gull {k + 1}"
        stra.behave(name, "Wheel", "Circle over the cove on the sea breeze, rising and falling, banking into the turn.", GULL_SRC)
        for side in ("R", "L"):
            stra.behave(f"{name} Wing {side}", "Flap", "Flap in bursts, then glide with the wings held slightly up.", WING_SRC)
    sx, sz = SHIP
    stra.behave("Brig Marigold", "Ride the swell", "Ride the simulated sea at anchor: heave with the water under the hull, pitch "
                "along the keel and roll across it, swinging a little on the anchor cable.", f"""
on tick
  let bow = water_height({sx} + 15, {sz} - 4)
  let stern = water_height({sx} - 15, {sz} + 4)
  let port = water_height({sx} + 1, {sz} + 3.5)
  let starboard = water_height({sx} - 1, {sz} - 3.5)
  let mid = (bow + stern + port + starboard) / 4
  self.position = ({sx}, mid * 0.85 - 0.15, {sz})
  self.rotation = ((port - starboard) * 3.5, 75 + sin(time * 0.07) * 2.5, (stern - bow) * 2.0)
end""")
    stra.behave("Pinnace Far Sail", "Sail", "Stand off the island, sailing slowly west to east on the horizon.", """
on tick
  let x = -260 + time * 1.6
  self.position = (x, water_height(x, 560) * 0.8 - 0.2, 560)
end""")
    stra.behave("Camp Lantern Light", "Flicker", "Flicker like a candle in the sea breeze.", """
on tick
  self.light.intensity = 2.2 + sin(time * 13 + 1.7) * 0.25 + random() * 0.35
end""")
    stra.flush("Swell, sail, flicker")


DUSK = dict(GOLDEN, sunElevation=2.6, sunAzimuth=18, sunIntensity=2.4, sunColor="#ffb884", ambient=0.5, fogColor="#a6a2b0",
            godRays=1.1, haze=0.004, exposureCompensation=0.1, look="teal_orange", lookStrength=0.3, saturation=1.0, clouds=0.5)

# The film's hero shots: a camera path (Catmull-Rom through `path`, each point [x, y, z] or
# [x, dy, z, "g"] = dy above the ground), a fixed look-at `target`, lens, mood and length.
SHOTS = [
    dict(name="establishing", mood="golden", seconds=7, fov=42,
         path=[[-95, 52, 480], [-62, 36, 420], [-38, 24, 372]], target=[18, 5, 215]),
    dict(name="swash", mood="golden", seconds=6, fov=38, aperture=5.6,
         path=[[-27, 0.75, 202.5, "g"], [-19, 0.7, 201.0, "g"], [-11, 0.75, 199.5, "g"]], target=[72, 2.5, 252]),
    dict(name="campfire", mood="dusk", seconds=7, fov=46, aperture=2.8, orbit=dict(center="camp", radius=4.6, height=1.05,
                                                                                    start=162, end=212, aim=0.55)),
    dict(name="watchtower", mood="golden", seconds=6, fov=46,
         path=[[90, 1.7, 196, "g"], [93, 3.0, 203, "g"], [95.5, 4.6, 209, "g"]], target=[104, 27, 238]),
    dict(name="brig", mood="golden", seconds=6, fov=36,
         path=[[-12, 1.7, 292], [0, 1.6, 290], [12, 1.7, 289]], target=[34, 9, 330]),
    dict(name="palms", mood="golden", seconds=6, fov=50,
         path=[[-46, 1.4, 150, "g"], [-43, 1.5, 154, "g"], [-40, 1.7, 158, "g"]], target=[-28, 9, 196]),
    dict(name="shallows", mood="midday", seconds=7, fov=48,
         path=[[-92, 34, 268], [-62, 30, 282], [-30, 28, 292]], target=[-24, 0, 236]),
    dict(name="dunes", mood="midday", seconds=6, fov=44, aperture=8,
         path=[[16, 1.65, 140, "g"], [14, 1.65, 147, "g"], [12, 1.7, 154, "g"]], target=[26, 2.5, 262]),
    dict(name="cliffs", mood="midday", seconds=6, fov=40,
         path=[[176, 4.5, 330], [182, 5.5, 300], [186, 6.5, 270]], target=[112, 12, 252]),
]
MOODS = {"golden": GOLDEN, "dusk": DUSK, "midday": MIDDAY}
SEQ_ENV = ("sunElevation", "sunAzimuth", "sunIntensity", "sunColor", "ambient", "clouds", "cloudDensity", "fogColor", "fogDensity",
           "godRays", "haze", "exposureCompensation", "look", "lookStrength", "saturation", "contrast", "bloomIntensity",
           "vignette", "shadowSoftness")


def _resolve_shot(world, shot):
    """Absolute camera path for a shot (ground-relative points resolved on the terrain)."""
    if "orbit" in shot:
        o = shot["orbit"]
        cx, cy, cz = world.spots[o["center"]]
        pts = []
        for k in range(5):
            a = math.radians(o["start"] + (o["end"] - o["start"]) * k / 4)
            pts.append([round(cx + math.sin(a) * o["radius"], 3), round(cy + o["height"], 3), round(cz + math.cos(a) * o["radius"], 3)])
        return pts, [cx, round(cy + o["aim"], 3), cz]
    pts = []
    for p in shot["path"]:
        if len(p) == 4:
            pts.append([p[0], round(world.y(p[0], p[2]) + p[1], 3), p[2]])
        else:
            pts.append(list(p))
    return pts, list(shot["target"])


def sequences(nim, world):
    """One cinematic sequence per hero shot: the mood (environment keys at t=0) and a path camera."""
    world.shots = {}
    for shot in SHOTS:
        pts, target = _resolve_shot(world, shot)
        world.shots[shot["name"]] = dict(path=pts, target=target, fov=shot["fov"], seconds=shot["seconds"], mood=shot["mood"],
                                         aperture=shot.get("aperture", 0))
        path = f"sequences/{shot['name']}.sequence.json"
        env = MOODS[shot["mood"]]
        tracks = [{"type": "property", "property": f"environment.{k}", "keys": [{"t": 0, "value": env[k], "ease": "step"}]}
                  for k in SEQ_ENV if k in env]
        nim.call("sequence_create", path=path, name=f"Tidebreak — {shot['name']}", duration=shot["seconds"], tracks=tracks,
                 entity=f"Sequence {shot['name']}", play_on_start=False, overwrite=True)
        cam = f"Cam {shot['name']}"
        nim.call("sequence_camera_shot", sequence=path, camera=cam, shot="path", start=0, duration=shot["seconds"], points=pts,
                 target=target, fov=shot["fov"], ease="smooth")
        nim.call("entity_update", entity=cam, components={"camera": {"farPlane": 5000, "nearPlane": 0.1,
                                                                     "aperture": shot.get("aperture", 0), "primary": False}})


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


def _shot_data():
    import json
    from sky import ROOT
    with open(os.path.join(ROOT, "examples", "tidebreak_isle", "shot_heights.json")) as f:
        return json.load(f)["shots"]


def shot_camera(shot, s):
    """Camera (eye, target, fov) of a resolved shot at normalized time s (smooth ease, like the sequence)."""
    e = s * s * (3 - 2 * s)
    return dict(eye=[round(v, 4) for v in _catmull(shot["path"], e)], target=shot["target"], fov=shot["fov"])


def shots():
    data = _shot_data()
    out = []
    for shot in SHOTS:
        d = data[shot["name"]]
        env = MOODS[d["mood"]]

        def cam(i, n, d=d):
            c = shot_camera(d, i / max(n - 1, 1))
            if d.get("aperture"):
                c["aperture"] = d["aperture"]
            return c

        def before(sky, i, n, env=env):
            if i == 0 or i == n // 2:
                sky.call("environment_update", **env)

        out.append(dict(name=shot["name"], frames=int(d["seconds"] * 30), cam=cam, before=before, warmup=120))
    return out


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Tidebreak Isle", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])

    # --- Pixel: photoscanned ground (Poly Haven, CC0) -----------------------------------------
    T = {k: ph.texture(pix, k, res=res, tiling=1.0, triplanar=False) for k, res in (
        ("aerial_beach_01", "4k"), ("damp_beach_sand_02", "2k"), ("aerial_beach_02", "2k"), ("low_tide_rocks", "2k"),
        ("forrest_sand_01", "2k"), ("aerial_grass_rock", "4k"), ("aerial_rocks_02", "4k"))}
    T["aerial_rocks_04"] = ph.texture(pix, "aerial_rocks_04", res="4k", tiling=1.0, triplanar=True)

    # --- Cirro: the island ----------------------------------------------------------------------
    # Tiling = the scans' real size in meters, so ripples, pebbles and cracks have true scale.
    layers = [
        {"name": "sand", "material": T["aerial_beach_01"], "tiling": 24.0, "color": [1.55, 1.45, 1.26]},
        {"name": "damp sand", "material": T["damp_beach_sand_02"], "tiling": 1.9, "color": [1.35, 1.25, 1.05], "heightMax": 0.55, "noise": 0.35, "sharpness": 0.3},
        {"name": "seabed", "material": T["aerial_beach_02"], "tiling": 20.0, "color": [1.7, 1.6, 1.35], "heightMax": -1.2, "noise": 0.5, "sharpness": 0.3},
        {"name": "tide rocks", "material": T["low_tide_rocks"], "tiling": 2.2, "heightMin": -2.0, "heightMax": 1.8, "slopeMin": 16,
         "noise": 0.7, "sharpness": 0.4},
        {"name": "sandy soil", "material": T["forrest_sand_01"], "tiling": 2.0, "heightMin": 4.4, "slopeMax": 40, "noise": 0.6,
         "sharpness": 0.3},
        {"name": "grass", "material": T["aerial_grass_rock"], "tiling": 15.0, "color": [0.72, 0.98, 0.55], "heightMin": 6.5, "slopeMax": 40, "noise": 0.7,
         "sharpness": 0.35},
        {"name": "mossy rock", "material": T["aerial_rocks_02"], "tiling": 25.0, "color": [0.92, 0.95, 0.85], "heightMin": 2.0, "slopeMin": 30, "noise": 0.5,
         "sharpness": 0.45, "triplanar": True},
        {"name": "cliff", "material": T["aerial_rocks_04"], "tiling": 18.0, "color": [1.15, 1.1, 1.05], "slopeMin": 40, "noise": 0.4, "sharpness": 0.55,
         "triplanar": True},
    ]
    cir.call("terrain_create", name="Island", preset="island_beach", size=640, resolution=1025, seed=3, water=True,
             generator={"beachWidth": 34, "maxHeight": 62, "featureSize": 320, "thermal": 0.35}, layers=layers)
    sculpt_island(cir)
    cir.call("terrain_layers", entity="Island", layers=layers)
    # The sea: a long swell from the south rolling into the cove, surf running up the sand.
    cir.call("entity_update", entity="Sea", components={"water": {
        "windSpeed": 6.0, "windDirection": 185, "choppiness": 1.1, "waveScale": 0.75, "patchSize": 180, "depth": 30,
        "deepColor": "#03303f", "shallowColor": "#3cd6d2", "clarity": 14, "foam": 1.0, "roughness": 0.03}})
    cir.call("entity_update", entity="Island", components={"terrain": {"waterLevel": SEA, "wetBand": 1.1, "detail": 1.5}})

    A = assets(pix)
    world = World(cir)
    vegetation(cir, A)
    rocks(cir, A, world)
    landmarks(pix, A, world)
    camp(pix, A, world)
    life(stra, A, world)

    aur.call("environment_update", **GOLDEN)
    nim.e("Player Camera", pos=(-30, world.y(-30, 170) + 1.7, 170), rot=(-4, 140, 0), camera={"fov": 55, "primary": True})
    nim.flush("Game camera")
    sequences(nim, world)
    nim.call("scene_save", path="scenes/main.sky.json")
    world.save(studio.project)
