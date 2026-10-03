"""Smuggler's Cove — a pirate adventure at golden hour, built from photoscanned CC0 assets
(Poly Haven) that the crew downloads with asset_download: galleons, sea cliffs, a pier, a shore battery,
treasure, lanterns, a photographed sunset sky and animated water."""
import math

import polyhaven as ph
from kit import rnd

META = dict(
    id="smugglers_cove", title="Smuggler's Cove", genre="Pirate Adventure", mood="golden hour",
    pitch="The galleon is in. The tide won't wait. Neither will the Navy.",
    view=dict(eye=[-30, 9, 48], target=[5, 4, -10]),
    assets="Poly Haven (CC0)",
)

# The island is a flattened ellipsoid: sand where it rises out of the sea; a grassy hill behind.
ISLAND = dict(c=(0.0, -8.0, -52.0), r=(92.0, 12.0, 50.0))
HILL = dict(c=(-34.0, -14.0, -82.0), r=(56.0, 26.0, 34.0))
CAMP = (2.0, -22.0)
PIER_X, DECK = -10.0, 1.7
SHIP = (22.0, 40.0)


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Smuggler's Cove", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])
    r = rnd(1717)

    # --- Pixel: fetch the assets (CC0, credited in CREDITS.md) -------------------------------
    A = {k: ph.model(pix, k) for k in (
        "dutch_ship_large_01", "dutch_ship_medium", "coastal_cliff_01",
        "cannon_01", "treasure_chest", "wooden_barrels_01", "wooden_crate_01", "wooden_crate_02", "wooden_lantern_01",
        "wooden_bucket_01", "fern_02", "rock_face_02", "boulder_01", "stone_fire_pit", "jacaranda_tree", "shrub_02")}
    sky = ph.hdri(pix, "wasteland_clouds_puresky")
    sand = ph.texture(pix, "coast_sand_01", tiling=0.12)
    turf = ph.texture(pix, "aerial_grass_rock", tiling=0.06)
    planks = ph.texture(pix, "old_planks_02", tiling=0.5)
    timber = ph.texture(pix, "rough_wood", tiling=0.8)
    # Cloth lets the low sun through; the hull keeps its photoscanned look.
    for ship in ("dutch_ship_large_01", "dutch_ship_medium"):
        pix.call("material_update", path=A[ship]["prefab"].replace(".prefab.json", "_sails.mat.json"), subsurface=0.7)

    # --- Cirro: land and sea first (everything else is placed on their real surface) ---------
    # A simulated sea (FFT ocean): swell rolling into the cove, surf on the sand.
    cir.call("fx_create", effect="calm_sea", name="Sea", position=[0, 0, 0],
             overrides={"windSpeed": 6.5, "windDirection": 200, "choppiness": 1.25, "deepColor": "#04202a",
                        "shallowColor": "#2aa898", "clarity": 7, "foam": 1.2})
    for name, e, mat in (("Island", ISLAND, sand), ("Hill", HILL, turf)):
        (cx, cy, cz), (rx, ry, rz) = e["c"], e["r"]
        cir.e(name, "sphere", pos=(cx, cy, cz), scale=(rx * 2, ry * 2, rz * 2), material=mat, tags=["terrain"])
    cir.flush("Island and sea")

    def ground(x, z):
        return ph.surface(cir, x, z)

    for k, (x, z, yaw, s) in enumerate([(-30, -112, 4, 2.3), (70, -104, -14, 2.0), (-118, -60, 68, 2.1)]):
        ph.place(cir, A["coastal_cliff_01"], f"Sea Cliffs {k + 1}", (x, -2.5, z), yaw=yaw, scale=s, tags=["cliff"])
    # The jetty: planked deck on timber posts, from the beach out to deep water.
    cir.e("Jetty", pos=(PIER_X, 0, 0), tags=["pier"])
    cir.e("Jetty Deck", "cube", pos=(0, DECK - 0.09, 9), scale=(3.0, 0.18, 52), parent="Jetty", material=planks)
    for side in (-1, 1):
        cir.e(f"Jetty Stringer {side}", "cube", pos=(side * 1.35, DECK - 0.35, 9), scale=(0.28, 0.32, 52), parent="Jetty",
              material=timber)
    for k in range(14):
        z = -16 + k * 4
        for side in (-1, 1):
            tall = k % 3 == 0
            cir.e(f"Jetty Post {k + 1}{'LR'[side > 0]}", "cylinder", pos=(side * 1.55, (DECK + (1.2 if tall else 0.25) - 2.5) / 2, z),
                  rot=(r.uniform(-2, 2), r.uniform(0, 360), r.uniform(-2, 2)),
                  scale=(0.32, DECK + (1.2 if tall else 0.25) + 2.5, 0.32), parent="Jetty", material=timber)
    cir.e("Jetty Ladder", "cube", pos=(1.75, DECK / 2 - 0.6, 33), scale=(0.6, DECK + 1.2, 0.08), parent="Jetty", material=timber)
    ph.place(cir, A["dutch_ship_large_01"], "The Gilded Gull", (SHIP[0], 0, SHIP[1]), yaw=200)
    ph.place(cir, A["dutch_ship_medium"], "Navy Sloop", (-95, 0, 120), yaw=70)
    ph.place(cir, A["jacaranda_tree"], "Old Jacaranda", (-28, ground(-28, -70) - 0.3, -70), yaw=40, scale=0.85)
    cir.flush("Cliffs, pier, ships, the old tree")

    # Rocks along the waterline, half sunk into sand and surf.
    (cx, cy, cz), (rx, ry, rz) = ISLAND["c"], ISLAND["r"]
    shore = rx * 0.745, rz * 0.745
    for k in range(18):
        a = math.radians(r.uniform(195, 345))
        f = r.uniform(0.92, 1.06)
        x, z = cx + math.cos(a) * shore[0] * f, cz - math.sin(a) * shore[1] * f
        if abs(x - PIER_X) < 6:  # keep the jetty clear
            continue
        kind = "boulder_01" if k % 3 else "rock_face_02"
        sc = r.uniform(1.4, 3.2)
        ph.place(cir, A[kind], f"Shore Rock {k + 1}", (x, max(ground(x, z), 0) - 0.35 * sc, z), yaw=r.uniform(0, 360),
                 scale=sc, tags=["rock"])
    for k in range(34):
        x, z = r.uniform(-70, 40), r.uniform(-90, -36)
        y = ground(x, z)
        if y < 1.0:
            continue
        kind = "fern_02" if k % 3 else "shrub_02"
        ph.place(cir, A[kind], f"Brush {k + 1}", (x, y - 0.1, z), yaw=r.uniform(0, 360), scale=r.uniform(1.0, 1.9),
                 tags=["plant"])
    cir.flush("Shore rocks and brush")

    # --- Pixel: the smugglers' camp and shore battery -----------------------------------------
    gx, gz = CAMP
    gy = ground(gx, gz)
    ph.place(pix, A["stone_fire_pit"], "Fire Pit", (gx, gy + 0.1, gz))
    ph.place(pix, A["treasure_chest"], "Treasure Chest", (gx - 3.2, ground(gx - 3.2, gz + 1.5), gz + 1.5), yaw=25, tags=["loot"])
    ph.place(pix, A["wooden_barrels_01"], "Rum Barrels", (-17, ground(-17, -22), -22), yaw=80)
    for k, (x, z, yaw) in enumerate([(-14.5, -18.5, 10), (-13.6, -19.6, 40), (6.0, -25, -20), (-3.5, -25, 70)]):
        kind = "wooden_crate_01" if k % 2 else "wooden_crate_02"
        ph.place(pix, A[kind], f"Crate {k + 1}", (x, ground(x, z), z), yaw=yaw)
    ph.place(pix, A["wooden_bucket_01"], "Bucket", (gx - 3.0, ground(gx - 3.0, gz - 2.5), gz - 2.5))
    for k, x in enumerate((14, 19, 24)):
        z = -26 + k * 0.6
        ph.place(pix, A["cannon_01"], f"Shore Cannon {k + 1}", (x, ground(x, z) - 0.05, z), yaw=200 - k * 8, tags=["cannon"])
    pix.flush("Smugglers' camp and shore battery")
    # Lanterns along the pier rail: warm pools of light at dusk, plus one at the camp.
    deck = DECK
    for k, z in enumerate([-16, -4, 8, 20, 32]):  # on the tall posts
        ph.place(pix, A["wooden_lantern_01"], f"Pier Lantern {k + 1}", (PIER_X + 1.55, DECK + 1.2, z), tags=["lantern"])
        pix.light(f"Pier Lantern Light {k + 1}", (PIER_X + 1.55, DECK + 1.55, z), "#ffb060", 3.0, range_=8, tags=["lantern_light"])
    ph.place(pix, A["wooden_lantern_01"], "Camp Lantern", (gx - 3.0, ground(gx - 3.2, gz + 1.5) + 0.62, gz + 1.4))
    pix.light("Camp Lantern Light", (gx - 3.0, gy + 1.1, gz + 1.4), "#ffb060", 2.2, range_=6)
    pix.flush("Lanterns")
    pix.call("fx_create", effect="campfire", name="Campfire", position=[gx, gy + 0.12, gz],
             overrides={"lightRange": 12})

    # --- Aurora: golden hour from a photographed sky --------------------------------------------
    aur.call("environment_update", skyMode="hdri", hdri=sky, hdriRotation=0, hdriIntensity=1.0, align_sun_to_hdri=True,
             sunColor="#ffc890", sunIntensity=3.6, ambient=0.45, reflections=1.0, fogColor="#c99d78", fogDensity=0.0007,
             fogHeight=0.05, exposure=1.0, bloomIntensity=0.35, bloomThreshold=1.6, tonemap="agx", saturation=1.15,
             contrast=1.12, vignette=0.28, temperature=0.15, ao=1.0, shadowSoftness=1.2, shadowDistance=180,
             showGrid=False)
    env = aur.call("environment_update")
    if env["sunElevation"] < 8:  # keep a low sun, but high enough to rake across the beach
        aur.call("environment_update", sunElevation=10)

    # --- Stratus: life ------------------------------------------------------------------------
    stra.behave("The Gilded Gull", "Ride the waves", "Float on the simulated sea: sit at the water height under the "
                "hull, pitch with the waves along the keel and roll with them across it.", f"""
on tick
  let bow = water_height({SHIP[0]} - 12, {SHIP[1]} - 4)
  let stern = water_height({SHIP[0]} + 12, {SHIP[1]} + 4)
  let port = water_height({SHIP[0]} - 1.5, {SHIP[1]} + 4)
  let starboard = water_height({SHIP[0]} + 1.5, {SHIP[1]} - 4)
  let mid = (bow + stern + port + starboard) / 4
  self.position = ({SHIP[0]}, mid * 0.8 - 0.1, {SHIP[1]})
  self.rotation = ((port - starboard) * 4, 200 + sin(time * 0.11) * 1.5, (bow - stern) * 2.2)
end""")
    stra.behave("Navy Sloop", "Patrol", "Sail slowly across the mouth of the cove, riding the waves.", """
on tick
  let x = -95 + time * 1.4
  let z = 120 - time * 0.35
  self.position = (x, water_height(x, z) * 0.8 - 0.1, z)
  self.rotation = ((water_height(x + 2, z) - water_height(x - 2, z)) * 6, 70, (water_height(x, z + 9) - water_height(x, z - 9)) * 3)
end""")
    for k in range(5):
        stra.behave(f"Pier Lantern Light {k + 1}", "Flicker", "Flicker like a candle in a sea breeze.", """
on tick
  self.light.intensity = 2.8 + sin(time * 11 + self.id) * 0.25 + random() * 0.5
end""")
    stra.flush("Swell, patrol, flicker")

    # Game camera: a third-person view down the pier toward the galleon.
    nim.e("Player Camera", pos=(-14, 4.5, -30), rot=(-8, 160, 0), camera={"fov": 52, "primary": True})
    nim.flush("Game camera")
    nim.call("scene_save", path="scenes/main.sky.json")
    # Heights for the shot cameras (cached next to the scene).
    import json
    import os
    with open(os.path.join(studio.project, "shot_heights.json"), "w") as f:
        json.dump({"camp": gy, "battery": ground(19, -25), "deck": deck}, f)


def _heights():
    import json
    import os
    from sky import ROOT
    with open(os.path.join(ROOT, "examples", "smugglers_cove", "shot_heights.json")) as f:
        return json.load(f)


def shots():
    h = _heights()
    gx, gz = CAMP

    def establish(i, n):  # drift in from the sea toward the cove
        t = i / (n - 1)
        e = t * t * (3 - 2 * t)
        return dict(eye=[70 - 35 * e, 16 - 6 * e, 110 - 45 * e], target=[0, 4, -35], fov=42)

    def galleon(i, n):  # low along the waterline past the hull
        t = i / (n - 1)
        return dict(eye=[-6 + 22 * t, 1.6, 60 - 4 * t], target=[SHIP[0], 7, SHIP[1]], fov=50)

    def pier(i, n):  # dolly down the pier at dusk
        t = i / (n - 1)
        return dict(eye=[PIER_X - 0.4, DECK + 1.7, -20 + 26 * t], target=[PIER_X + 3, DECK + 0.4, 40 + 6 * t], fov=55)

    def camp(i, n):  # orbit the treasure and the fire
        t = i / (n - 1)
        a = math.radians(70 + 50 * t)
        return dict(eye=[gx + math.cos(a) * 7, h["camp"] + 1.8, gz + math.sin(a) * 7], target=[gx - 1.5, h["camp"] + 0.5, gz + 0.5],
                    fov=45)

    def battery(i, n):  # behind the cannons, out to the galleon
        t = i / (n - 1)
        return dict(eye=[22 - 5 * t, h["battery"] + 1.6, -31.5 + 1.5 * t], target=[SHIP[0] - 4, 2.5, SHIP[1]], fov=42)

    return [dict(name="establish", frames=180, cam=establish, warmup=60),
            dict(name="galleon", frames=150, cam=galleon),
            dict(name="pier", frames=150, cam=pier),
            dict(name="camp", frames=150, cam=camp),
            dict(name="battery", frames=135, cam=battery)]
