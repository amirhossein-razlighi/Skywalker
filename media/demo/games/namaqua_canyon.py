"""Namaqua Canyon — open-world survival at sunset. Photoscanned CC0 cliffs, boulders, quiver
trees and succulents (Poly Haven's Namaqualand collection), a photographed sky, a survivor's
camp around a simulated campfire, and sun shafts through the dust."""
import math

import polyhaven as ph
from kit import rnd

META = dict(
    id="namaqua_canyon", title="Namaqua Canyon", genre="Open-world Survival", mood="sunset",
    pitch="Water's low, the jeep's covered, and the sun is going down fast.",
    view=dict(eye=[24, 9, 46], target=[0, 3, -10]),
    assets="Poly Haven (CC0)",
)

CAMP = (4.0, 6.0)


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Namaqua Canyon", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])
    r = rnd(2718)

    # --- Pixel: assets (CC0) ---------------------------------------------------------------
    A = {k: ph.model(pix, k) for k in (
        "namaqualand_cliff_01", "namaqualand_cliff_02", "mountainside", "namaqualand_boulder_02", "namaqualand_boulder_03",
        "namaqualand_boulder_04", "namaqualand_boulder_05", "namaqualand_boulders_01", "namaqualand_rocks_01",
        "quiver_tree_01", "quiver_tree_02", "dead_quiver_trunk", "flower_gazania", "flower_ursinia", "crystalline_iceplant",
        "cheiridopsis_succulent", "dry_branches_medium_01", "stone_fire_pit", "wooden_crate_02", "metal_jerrycan",
        "propane_tank", "binoculars", "bolt_action_rifle_7_62", "covered_car", "rock_face_02")}
    sky = ph.hdri(pix, "table_mountain_2_puresky")
    sand = ph.texture(pix, "red_sand", tiling=0.15)
    ground = ph.texture(pix, "dry_ground_rocks", tiling=0.12)

    # --- Cirro: the canyon ---------------------------------------------------------------------
    cir.e("Desert Floor", "plane", pos=(0, 0, 0), scale=(900, 1, 900), material=sand, tags=["terrain"])
    for k in range(9):
        x, z = r.uniform(-140, 140), r.uniform(-260, -40)
        cir.e(f"Dune {k + 1}", "sphere", pos=(x, -9, z), scale=(r.uniform(60, 120), 24, r.uniform(40, 80)), material=sand,
              tags=["terrain"])
    for k in range(5):
        cir.e(f"Gravel Patch {k + 1}", "cylinder", pos=(r.uniform(-12, 12), -0.48, r.uniform(-60, 30)),
              scale=(r.uniform(8, 16), 1.0, r.uniform(8, 16)), material=ground, tags=["terrain"])
    cir.flush("Desert floor, dunes and the dry wash")

    # Canyon walls: photoscanned cliffs scaled up, layered left and right; peaks beyond.
    walls = [(-30, -6, 82, 2.6), (-34, -48, 76, 3.0), (-28, 36, 95, 2.4), (30, -14, -96, 2.8), (36, -56, -84, 3.1),
             (32, 30, -88, 2.5), (-12, -110, 10, 3.6), (18, -118, -6, 3.4)]
    for k, (x, z, yaw, s) in enumerate(walls):
        kind = "namaqualand_cliff_02" if k % 3 else "namaqualand_cliff_01"
        sc = s * (1.0 if kind == "namaqualand_cliff_02" else 2.2)
        ph.place(cir, A[kind], f"Canyon Wall {k + 1}", (x, -0.6, z), yaw=yaw, scale=sc, tags=["cliff"])
    for k, (x, z, yaw, s) in enumerate([(-70, -170, 20, 6.5), (40, -190, -30, 7.5), (110, -150, -60, 6.0), (-130, -120, 50, 5.5)]):
        ph.place(cir, A["mountainside"], f"Peak {k + 1}", (x, -2, z), yaw=yaw, scale=s, tags=["mountain"])
    cir.flush("Canyon walls and peaks")

    def surf(x, z):
        return ph.surface(cir, x, z)

    # Boulders, quiver trees, succulents and flowers (the Namaqualand spring after rain).
    boulders = ("namaqualand_boulder_02", "namaqualand_boulder_03", "namaqualand_boulder_04", "namaqualand_boulder_05")
    for k in range(26):
        side = -1 if k % 2 else 1
        x, z = side * r.uniform(9, 26), r.uniform(-80, 40)
        kind = boulders[k % 4]
        ph.place(cir, A[kind], f"Boulder {k + 1}", (x, surf(x, z) - 0.15, z), yaw=r.uniform(0, 360), scale=r.uniform(1.2, 3.4),
                 tags=["rock"])
    for k in range(10):
        x, z = r.uniform(-22, 22), r.uniform(-70, 40)
        if abs(x - CAMP[0]) < 5 and abs(z - CAMP[1]) < 5:
            continue
        ph.place(cir, A["namaqualand_rocks_01" if k % 2 else "namaqualand_boulders_01"], f"Rubble {k + 1}",
                 (x, surf(x, z), z), yaw=r.uniform(0, 360), scale=r.uniform(1.5, 3.0))
    trees = [(-8, -4), (12, -18), (-15, -30), (7, 22), (-6, 30), (16, 4), (-18, 10), (3, -45), (-11, -58), (14, -38)]
    for k, (x, z) in enumerate(trees):
        kind = "quiver_tree_01" if k % 3 else "quiver_tree_02"
        sc = r.uniform(1.6, 2.4) if kind == "quiver_tree_01" else r.uniform(2.4, 3.2)
        ph.place(cir, A[kind], f"Quiver Tree {k + 1}", (x, surf(x, z) - 0.05, z), yaw=r.uniform(0, 360), scale=sc, tags=["tree"])
    for k, (x, z) in enumerate([(-3, -12), (10, 14), (-14, 20)]):
        ph.place(cir, A["dead_quiver_trunk"], f"Dead Trunk {k + 1}", (x, surf(x, z), z), rot=(r.uniform(-12, 12), r.uniform(0, 360), 80),
                 scale=2.0)
    flora = ("flower_gazania", "flower_ursinia", "crystalline_iceplant", "cheiridopsis_succulent", "dry_branches_medium_01")
    for k in range(60):
        x, z = r.uniform(-24, 24), r.uniform(-75, 42)
        if abs(x - CAMP[0]) < 3.5 and abs(z - CAMP[1]) < 3.5:
            continue
        kind = flora[k % len(flora)]
        ph.place(cir, A[kind], f"Plant {k + 1}", (x, surf(x, z) - 0.02, z), yaw=r.uniform(0, 360), scale=r.uniform(1.0, 1.8),
                 tags=["plant"])
    cir.flush("Boulders, quiver trees and spring flowers")

    # --- Pixel: the survivor's camp ---------------------------------------------------------
    cx, cz = CAMP
    gy = surf(cx, cz)
    ph.place(pix, A["stone_fire_pit"], "Fire Pit", (cx, gy + 0.12, cz))
    ph.place(pix, A["covered_car"], "Covered Jeep", (cx + 5.5, surf(cx + 5.5, cz - 2), cz - 2), yaw=-35)
    ph.place(pix, A["wooden_crate_02"], "Supply Crate", (cx - 2.4, surf(cx - 2.4, cz + 1.2), cz + 1.2), yaw=20)
    ph.place(pix, A["wooden_crate_02"], "Supply Crate 2", (cx - 2.9, surf(cx - 2.9, cz + 0.2), cz + 0.2), yaw=-15)
    crate_top = surf(cx - 2.4, cz + 1.2) + 0.45
    ph.place(pix, A["bolt_action_rifle_7_62"], "Rifle", (cx - 2.4, crate_top + 0.04, cz + 1.2), rot=(0, 110, 90))
    ph.place(pix, A["binoculars"], "Binoculars", (cx - 2.2, crate_top + 0.02, cz + 1.0), yaw=40)
    ph.place(pix, A["metal_jerrycan"], "Jerrycan", (cx + 3.6, surf(cx + 3.6, cz + 0.6), cz + 0.6), yaw=70)
    ph.place(pix, A["metal_jerrycan"], "Jerrycan 2", (cx + 3.9, surf(cx + 3.9, cz + 1.0), cz + 1.0), yaw=95)
    ph.place(pix, A["propane_tank"], "Propane", (cx - 1.6, surf(cx - 1.6, cz - 1.8), cz - 1.8))
    ph.place(pix, A["namaqualand_boulder_04"], "Windbreak Rock", (cx - 4.8, surf(cx - 4.8, cz - 3.5) - 0.3, cz - 3.5), yaw=40,
             scale=1.6)
    pix.flush("The camp")
    pix.call("fx_create", effect="campfire", name="Campfire", position=[cx, gy + 0.18, cz],
             overrides={"lightRange": 12})

    # --- Aurora: last light ----------------------------------------------------------------
    ph.aim_sun(aur, sky, 195.0)  # low sun ahead of the cameras: backlit trees and rock
    aur.call("environment_update",
             sunColor="#ffb878", sunIntensity=3.6, ambient=0.45, reflections=0.8, fogColor="#d8a07a", fogDensity=0.0008,
             fogHeight=0.05, exposure=1.05, bloomIntensity=0.35, bloomThreshold=1.5, tonemap="agx", saturation=1.12,
             contrast=1.1, vignette=0.3, temperature=0.15, ao=1.0, shadowSoftness=1.2, shadowDistance=220,
             godRays=1.2, haze=0.0035, windSpeed=3, windDirection=250, showGrid=False)
    env = aur.call("environment_update")
    if env["sunElevation"] < 5 or env["sunElevation"] > 20:
        aur.call("environment_update", sunElevation=9)
    aur.call("fx_create", effect="dust", name="Drifting Dust", position=[0, 1.5, -10],
             overrides={"shapeSize": [40, 3, 90], "rate": 120, "maxParticles": 1500, "colorStart": "#ffe2b800",
                        "colorEnd": "#ffe2b800", "intensity": 0.7, "wind": 0.6})

    stra.behave("Drifting Dust", "Gusts", "The wind gusts every few seconds, lifting more dust.", """
on tick
  self.particles.rate = 120 + max(0, sin(time * 0.7)) * 220
end""")
    stra.flush("Gusts")

    nim.e("Player Camera", pos=(cx + 6, gy + 2.2, cz + 9), rot=(-6, 30, 0), camera={"fov": 55, "primary": True})
    nim.flush("Game camera")
    nim.call("scene_save", path="scenes/main.sky.json")
    import json
    import os
    with open(os.path.join(studio.project, "shot_heights.json"), "w") as f:
        json.dump({"camp": gy}, f)


def _h():
    import json
    import os
    from sky import ROOT
    with open(os.path.join(ROOT, "examples", "namaqua_canyon", "shot_heights.json")) as f:
        return json.load(f)


def shots():
    h = _h()
    cx, cz = CAMP

    def aerial(i, n):  # sweep over the canyon toward the sun
        t = i / (n - 1)
        e = t * t * (3 - 2 * t)
        return dict(eye=[28 - 20 * e, 22 - 8 * e, 70 - 40 * e], target=[0, 2, -40], fov=45)

    def trees(i, n):  # low through the quiver trees
        t = i / (n - 1)
        return dict(eye=[-3 + 6 * t, 1.5, 34 - 14 * t], target=[-2 + 4 * t, 2.8, -30], fov=42)

    def camp(i, n):  # orbit the fire
        t = i / (n - 1)
        a = math.radians(40 + 55 * t)
        return dict(eye=[cx + math.cos(a) * 6.5, h["camp"] + 1.6, cz + math.sin(a) * 6.5], target=[cx - 0.5, h["camp"] + 0.7, cz],
                    fov=48)

    def sunset(i, n):  # wide, the canyon silhouetted
        t = i / (n - 1)
        return dict(eye=[1 + 3 * t, 5.0 + t, 62], target=[0, 6, -70], fov=36)

    return [dict(name="aerial", frames=180, cam=aerial, warmup=60),
            dict(name="trees", frames=150, cam=trees),
            dict(name="camp", frames=150, cam=camp),
            dict(name="sunset", frames=135, cam=sunset)]
