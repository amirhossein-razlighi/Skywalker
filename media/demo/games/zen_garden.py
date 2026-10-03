"""Zen Garden — a meditative garden in morning mist. Rake, wait, listen."""
import math
from kit import rnd

META = dict(
    id="zen_garden", title="Zen Garden", genre="Meditative · Art Game", mood="soft",
    pitch="Rake the sand. Feed the koi. Let the petals fall.",
    view=dict(eye=[12, 7, 16], target=[0, 1, -1]),
)


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Zen Garden", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])
    r = rnd(8)

    sand = pix.texture("sand", "raked_sand", color1="#e8dcc4", color2="#d4c6aa", color3="#bfae90", scale=10, bump=1.4, tiling=0.35)
    moss = pix.texture("grass", "garden_moss", color1="#5f7a3a", color2="#4a6230", color3="#7a8a48", tiling=0.4)
    stone = pix.texture("rock", "garden_stone", color1="#5a5a58", color2="#3e3e3c", color3="#6a7050", tiling=0.7)
    wood = pix.texture("wood", "cedar", color1="#8a5a3a", color2="#6a4028", color3="#4a2a18", tiling=1.0)
    pix.material("materials/lacquer.mat.json", color="#c0281e", roughness=0.25, clearcoat=1)
    pix.material("materials/pond_water.mat.json", preset="water", color="#3a8a9a88")
    pix.material("materials/blossom.mat.json", color="#f6b8cc", roughness=0.7, subsurface=0.7)
    # Bamboo cluster with its own sway
    pix.e("Bamboo")
    for k in range(5):
        a = k * 1.3
        h = 6 + (k % 3) * 1.5
        pix.e(f"Bamboo Cane {k+1}", "cylinder", "#6a9a3a", (math.cos(a) * 0.35, h / 2, math.sin(a) * 0.35), scale=(0.16, h, 0.16),
              parent="Bamboo", roughness=0.45)
        for j in range(int(h)):
            pix.e(f"Bamboo Node {k+1}.{j+1}", "cylinder", "#56802e", (math.cos(a) * 0.35, 0.8 + j, math.sin(a) * 0.35),
                  scale=(0.19, 0.06, 0.19), parent="Bamboo")
        for j in range(3):
            pix.e(f"Bamboo Leaf {k+1}.{j+1}", "sphere", "#7aa848", (math.cos(a) * 0.5, h - 0.3 - j * 0.5, math.sin(a) * 0.5),
                  rot=(20, a * 57 + j * 120, 35), scale=(0.9, 0.04, 0.14), parent="Bamboo", roughness=0.7, subsurface=0.6,
                  doubleSided=True)
    pix.flush("Bamboo cluster")
    pix.behave("Bamboo", "Sway", "Sway gently in the breeze, each cluster on its own phase.", """
on tick
  self.rotation = (sin(time * 0.7 + self.id) * 2.5, 0, cos(time * 0.55 + self.id * 1.3) * 2.5)
end""")
    pix.flush("Bamboo sway")
    pix.prefab("Bamboo", "prefabs/bamboo.prefab.json", "Swaying bamboo cluster", ["garden", "plant"])

    # --- Cirro: the garden --------------------------------------------------------------------
    cir.e("Moss", "plane", pos=(0, -0.02, 0), scale=(300, 1, 300), material=moss, tags=["terrain"])
    cir.e("Sand Bed", "cube", pos=(2, 0.0, 2), scale=(22, 0.12, 16), material=sand, tags=["terrain"])
    for k in range(28):
        a = 2 * math.pi * k / 28
        x, z = 2 + math.cos(a) * 11.6, 2 + math.sin(a) * 8.6
        x = max(-9.3, min(13.3, x))
        z = max(-6.3, min(10.3, z))
        cir.e(f"Border Stone {k+1}", "sphere", pos=(x, 0.05, z), rot=(0, r.uniform(0, 90), 0),
              scale=(r.uniform(0.7, 1.1), 0.35, r.uniform(0.6, 0.9)), material=stone)
    for k, (x, z, s) in enumerate([(-2, 0, 1.6), (5, 5, 1.2), (8, -2, 0.9)]):
        cir.e(f"Rock {k+1}", "sphere", pos=(x, s * 0.25, z), rot=(r.uniform(-10, 10), r.uniform(0, 360), 0),
              scale=(s * 1.6, s * 1.1, s * 1.3), material=stone, tags=["rock"])
        cir.e(f"Rock {k+1} Moss", "sphere", "#5f7a3a", (x - s * 0.1, s * 0.55, z), scale=(s * 1.0, s * 0.3, s * 0.8), roughness=1)
        cir.e(f"Rake Ring {k+1}", "torus", "#e2d6bc", (x, 0.02, z), scale=(s * 2.6, 0.08, s * 2.3), roughness=0.95)
    cir.flush("Sand bed, rocks, rake rings")

    # Koi pond
    cir.e("Pond Basin", "cylinder", "#2a3a3a", (-14, -0.8, 4), scale=(10, 0.8, 8), roughness=0.9)
    for k in range(22):
        a = 2 * math.pi * k / 22
        cir.e(f"Pond Stone {k+1}", "sphere", pos=(-14 + math.cos(a) * 5.1, 0.05, 4 + math.sin(a) * 4.1),
              rot=(0, r.uniform(0, 180), 0), scale=(r.uniform(0.8, 1.2), 0.45, r.uniform(0.7, 1.0)), material=stone)
    cir.e("Pond Water", "cylinder", pos=(-14, -0.02, 4), scale=(9.8, 0.04, 7.8), material="materials/pond_water.mat.json", tags=["water"])
    for k in range(7):
        c = ["#ff7a2a", "#ffffff", "#ff5a1a", "#f4c040", "#ff7a2a", "#ffffff", "#e84a2a"][k]
        koi = f"Koi {k+1}"
        cir.e(koi, pos=(-14, -0.35, 4), tags=["koi"], vars={"r": 1.4 + (k % 3) * 0.9, "speed": 0.35 + (k % 4) * 0.08, "phase": k * 0.9})
        cir.e(f"{koi} Body", "capsule", c, (0, 0, 0), rot=(90, 0, 0), scale=(0.5, 1.1, 0.35), parent=koi, roughness=0.3)
        cir.e(f"{koi} Spot", "sphere", "#ffffff" if c != "#ffffff" else "#ff6a2a", (0, 0.06, 0.1), scale=(0.2, 0.08, 0.28), parent=koi)
        cir.e(f"{koi} Tail", "cone", c, (0, 0, 0.62), rot=(-90, 0, 0), scale=(0.35, 0.35, 0.06), parent=koi)
    for k in range(9):
        a = r.uniform(0, 6.28)
        d = r.uniform(1, 3.4)
        cir.e(f"Lily Pad {k+1}", "cylinder", "#4f8a3a", (-14 + math.cos(a) * d * 1.2, 0.02, 4 + math.sin(a) * d),
              scale=(r.uniform(0.6, 1.0), 0.02, r.uniform(0.6, 1.0)), roughness=0.5)
    for k in range(2):
        cir.e(f"Lotus {k+1}", "sphere", "#f6b8cc", (-15 + k * 2.5, 0.12, 3 + k * 1.5), scale=(0.35, 0.22, 0.35), subsurface=0.8)
    for k in range(9):
        t = k / 8
        x = -14 + (t - 0.5) * 9
        y = 0.3 + math.sin(t * math.pi) * 0.9
        slope = 0.9 * math.pi * math.cos(t * math.pi) / 9.0
        cir.e(f"Bridge Plank {k+1}", "cube", pos=(x, y, 4), rot=(0, 0, math.degrees(math.atan(slope))),
              scale=(1.1, 0.12, 1.6), material=wood)
    cir.e("Bridge Rail L", "cube", "#c0281e", (-14, 1.35, 3.2), scale=(9, 0.08, 0.08), material="materials/lacquer.mat.json")
    cir.e("Bridge Rail R", "cube", "#c0281e", (-14, 1.35, 4.8), scale=(9, 0.08, 0.08), material="materials/lacquer.mat.json")
    cir.flush("Koi pond and bridge")

    # Torii, lanterns, cherry tree
    cir.e("Torii", pos=(4, 0, -9))
    for s in (-1, 1):
        cir.e(f"Torii Pillar {s}", "cylinder", pos=(s * 2.4, 2.4, 0), scale=(0.45, 4.8, 0.45), parent="Torii", material="materials/lacquer.mat.json")
    cir.e("Torii Kasagi", "cube", "#1a1a1a", (0, 5.0, 0), scale=(7.2, 0.4, 0.7), parent="Torii", roughness=0.5)
    cir.e("Torii Shimaki", "cube", pos=(0, 4.6, 0), scale=(6.6, 0.35, 0.5), parent="Torii", material="materials/lacquer.mat.json")
    cir.e("Torii Nuki", "cube", pos=(0, 3.7, 0), scale=(5.8, 0.28, 0.3), parent="Torii", material="materials/lacquer.mat.json")
    for k, (x, z) in enumerate([(-6, -4), (11, 8), (-9, 9)]):
        lan = f"Lantern {k+1}"
        cir.e(lan, pos=(x, 0, z))
        for name, y, sc, mesh in [("Base", 0.3, (0.7, 0.6, 0.7), "cylinder"), ("Post", 0.9, (0.3, 0.8, 0.3), "cylinder"),
                                  ("Light Box", 1.6, (0.7, 0.6, 0.7), "cube"), ("Roof", 2.1, (1.3, 0.5, 1.3), "cone"),
                                  ("Jewel", 2.45, (0.22, 0.22, 0.22), "sphere")]:
            cir.e(f"{lan} {name}", mesh, pos=(0, y, 0), scale=sc, parent=lan, material=stone)
        cir.e(f"{lan} Glow", "cube", "#000000", (0, 1.6, 0), scale=(0.72, 0.36, 0.5), parent=lan, emissive=[1, 0.72, 0.4, 2.2], unlit=True)
    cir.e("Cherry Tree", pos=(9, 0, 1))
    trunk = [((0, 1.2, 0), (0, 0, 8), (0.55, 2.6, 0.55)), ((-0.4, 3.0, 0.1), (0, 0, 28), (0.4, 2.2, 0.4)),
             ((0.6, 3.2, -0.2), (12, 0, -30), (0.35, 2.4, 0.35)), ((-1.3, 4.0, 0.3), (0, 0, 55), (0.25, 1.8, 0.25))]
    for k, (p, rt, sc) in enumerate(trunk):
        cir.e(f"Cherry Branch {k+1}", "cylinder", "#3a2620", p, rot=rt, scale=sc, parent="Cherry Tree", roughness=0.9)
    for k in range(22):
        a = r.uniform(0, 6.28)
        d = r.uniform(0.3, 2.6)
        cir.e(f"Blossom {k+1}", "sphere", pos=(math.cos(a) * d - 0.2, 4.4 + r.uniform(-0.6, 1.0), math.sin(a) * d * 0.8),
              scale=r.uniform(1.1, 1.9), parent="Cherry Tree", material="materials/blossom.mat.json")
    for k in range(70):
        cir.e(f"Petal {k+1}", "sphere", "#f8c4d4", (9 + r.uniform(-4, 4), r.uniform(0, 6), 1 + r.uniform(-4, 4)),
              scale=(0.09, 0.02, 0.07), subsurface=0.8, tags=["petal"], vars={"drift": r.uniform(0.3, 0.8)})
    cir.flush("Torii, lanterns, cherry tree, petals")
    cir.call("scatter", prefab="prefabs/bamboo.prefab.json", count=22, center=[0, 0, -16], size=[44, 7], min_distance=1.8,
             scale=[0.8, 1.3], seed=31, surface="Moss", group="Bamboo Grove")

    # --- Aurora: morning mist ----------------------------------------------------------------------
    aur.call("environment_update", skyMode="atmosphere", clouds=0.3, skyTop="#9ab8e0", skyHorizon="#f4dce4", ground="#7a7060",
             ambient=0.5, sunAzimuth=110, sunElevation=14, sunColor="#ffd8c4", sunIntensity=2.2, sunSize=1.4,
             fogColor="#f0dce0", fogDensity=0.016, fogHeight=0.45, exposure=1.05, showGrid=False, bloomIntensity=0.45,
             bloomThreshold=1.0, saturation=0.92, contrast=0.96, vignette=0.25, tonemap="agx", ao=1.1, temperature=0.1, tint=0.12)
    for k, (x, z) in enumerate([(-6, -4), (11, 8), (-9, 9)]):
        aur.light(f"Lantern Light {k+1}", (x, 1.6, z), "#ffbe7a", 1.4, 4)
    aur.flush("Morning light and lantern glow")

    # --- Stratus -------------------------------------------------------------------------------------
    for k in range(7):
        stra.behave(f"Koi {k+1}", "Swim", "Glide in slow overlapping circles under the water.", """
on tick
  let a = time * self.speed + self.phase
  self.position = (-14 + cos(a) * self.r * 1.25, -0.35 + sin(a * 2) * 0.05, 4 + sin(a) * self.r)
  self.rotation = (0, 0 - a * 57.3, sin(time * 3 + self.phase) * 6)
end""")
    for k in range(70):
        stra.behave(f"Petal {k+1}", "Fall", "Flutter down from the cherry tree, drifting with the breeze, then start again.", """
on tick
  move self by ((0.25 + sin(time * 0.9 + self.id) * 0.3) * self.drift * dt, -0.32 * dt, sin(time * 0.7 + self.id) * 0.2 * dt)
  rotate self by (90 * dt, 60 * dt, 120 * dt)
  if self.position.y < 0.08 then
    self.position = (9 + random(-3, 3), random(4.5, 6), 1 + random(-3, 3))
  end
end""")
    stra.e("Game Camera", pos=(12, 5, 15), camera={"fov": 45, "primary": True})
    stra.flush("Koi, petals")
    stra.behave("Game Camera", "Contemplate", "A slow, contemplative drift around the garden.", """
on tick
  let a = time * 0.05 + 0.7
  self.position = (2 + cos(a) * 17, 5.5 + sin(time * 0.1) * 0.6, 2 + sin(a) * 15)
  look self at (0, 1.2, 1)
end""")
    stra.flush("Camera")
    nim.call("viewport_multi", size=256)
    nim.call("scene_save", path="scenes/main.sky.json")


def shots():
    def pond(i, n):
        t = i / (n - 1)
        return dict(eye=[-8 - 2 * t, 3.2 - 0.6 * t, 10 - t], target=[-14, -0.2, 4])

    def blossom(i, n):
        t = i / (n - 1)
        return dict(eye=[14 - 3 * t, 1.2 + 0.4 * t, 6 - 2 * t], target=[8, 4.2, 0])

    def torii(i, n):
        t = i / (n - 1)
        e = t * t * (3 - 2 * t)
        return dict(eye=[4, 1.8, 6 - 6 * e], target=[4, 3.0, -16])

    return [dict(name="pond", frames=150, cam=pond),
            dict(name="blossom", frames=135, cam=blossom),
            dict(name="torii", frames=150, cam=torii),
            dict(name="play", frames=150, view="scene")]
