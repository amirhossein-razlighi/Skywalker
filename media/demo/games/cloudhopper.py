"""Cloudhopper — a bright 3D platformer across a ring of floating islands."""
import math
from kit import rnd

META = dict(
    id="cloudhopper", title="Cloudhopper", genre="3D Platformer", mood="bright",
    pitch="Six islands, one bouncy hero, every coin in the sky.",
    view=dict(eye=[0, 22, 34], target=[0, 0, 0]),
)

STYLE = {"shading": "toon", "outline": 2.2, "rim": 0.35}  # every mesh: cel-shaded with ink outlines
RING, N, W = 14.0, 6, 0.33  # ring radius, islands, angular speed (rad/s)
GREEN = ["#5ccf4a", "#4fc243", "#67d655"]


def island(b, name, x, z, radius, y=0.0, seed=1):
    r = rnd(seed)
    b.e(name, pos=(x, y, z), tags=["island"])
    b.e(f"{name} Grass", "cylinder", r.choice(GREEN), (0, -0.25, 0), scale=(radius * 2, 0.5, radius * 2), parent=name, roughness=0.9)
    b.e(f"{name} Soil", "cylinder", "#a9714a", (0, -0.85, 0), scale=(radius * 1.94, 0.7, radius * 1.94), parent=name, roughness=1)
    b.e(f"{name} Rock", "cone", "#8a5a3a", (0, -1.2 - radius * 0.9, 0), rot=(180, 0, 0), scale=(radius * 1.9, radius * 1.8, radius * 1.9),
        parent=name, roughness=1)
    for k in range(3):
        a = r.uniform(0, 6.28)
        d = r.uniform(0.3, 0.75) * radius
        b.e(f"{name} Flower {k+1}", "sphere", r.choice(["#ff5a7a", "#ffd23a", "#ffffff", "#b77aff"]),
            (math.cos(a) * d, 0.12, math.sin(a) * d), scale=0.22, parent=name, roughness=0.6)


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Cloudhopper", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])
    r = rnd(5)

    pix.material("materials/coin.mat.json", color="#ffc61a", metallic=1, roughness=0.2, emissive=[1, 0.7, 0.1, 0.5])
    pix.e("Mushroom")
    pix.e("Mushroom Stem", "cylinder", "#f6eedc", (0, 0.35, 0), scale=(0.32, 0.7, 0.32), parent="Mushroom", roughness=0.8)
    pix.e("Mushroom Cap", "sphere", "#e8392f", (0, 0.75, 0), scale=(0.95, 0.55, 0.95), parent="Mushroom", roughness=0.4)
    for k in range(5):
        a = k * 1.256
        pix.e(f"Mushroom Dot {k+1}", "sphere", "#ffffff", (math.cos(a) * 0.32, 0.93, math.sin(a) * 0.32), scale=(0.18, 0.08, 0.18), parent="Mushroom")
    pix.flush("Mushroom")
    pix.prefab("Mushroom", "prefabs/mushroom.prefab.json", "Red spotted mushroom", ["platformer", "decor"])
    pix.e("Round Tree")
    pix.e("Round Tree Trunk", "cylinder", "#8a5a3a", (0, 0.7, 0), scale=(0.3, 1.4, 0.3), parent="Round Tree")
    pix.e("Round Tree Top", "sphere", "#3fae3f", (0, 2.0, 0), scale=(1.8, 1.7, 1.8), parent="Round Tree", roughness=0.8)
    pix.flush("Round tree")
    pix.prefab("Round Tree", "prefabs/round_tree.prefab.json", "Cartoon round tree", ["platformer", "nature"])

    # --- Cirro: the course ------------------------------------------------------------------
    for k in range(N):
        a = 2 * math.pi * (k + 0.5) / N
        island(cir, f"Isle {k+1}", math.cos(a) * RING, math.sin(a) * RING, 4.6, seed=k + 1)
    island(cir, "Castle Isle", 0, 0, 6.5, y=2.5, seed=99)
    cir.flush("Six islands around a castle isle")
    for k in range(N):
        a = 2 * math.pi * (k + 0.5) / N
        x, z = math.cos(a) * RING, math.sin(a) * RING
        cir.op("prefab_instantiate", prefab="prefabs/round_tree.prefab.json", position=[x + math.cos(a) * 2.6, 0, z + math.sin(a) * 2.6],
               scale=r.uniform(0.8, 1.2), name=f"Tree {k+1}")
        cir.op("prefab_instantiate", prefab="prefabs/mushroom.prefab.json", position=[x - math.sin(a) * 2.2, 0, z + math.cos(a) * 2.2],
               scale=r.uniform(0.8, 1.4), name=f"Mushroom {k+1}")
        # Coins along the jump arc over the gap after this island
        g = 2 * math.pi * (k + 1) / N
        for c in range(3):
            ga = g + (c - 1) * 0.07
            cir.e(f"Coin {k+1}.{c+1}", "torus", pos=(math.cos(ga) * RING, 1.6 + (1.2 if c == 1 else 0.9), math.sin(ga) * RING),
                  rot=(90, 0, 0), scale=(0.9, 0.9, 0.9), material="materials/coin.mat.json", tags=["coin"])
    cir.flush("Trees, mushrooms, coins")

    # Castle on the center isle
    cir.e("Castle", pos=(0, 2.5, 0))
    cir.e("Keep", "cube", "#f1ead8", (0, 2.2, 0), scale=(4, 4.4, 4), parent="Castle", roughness=0.7)
    for k, (x, z) in enumerate([(-2.2, -2.2), (2.2, -2.2), (-2.2, 2.2), (2.2, 2.2)]):
        cir.e(f"Turret {k+1}", "cylinder", "#f1ead8", (x, 2.8, z), scale=(1.5, 5.6, 1.5), parent="Castle", roughness=0.7)
        cir.e(f"Turret {k+1} Roof", "cone", "#3a6fe0", (x, 6.4, z), scale=(1.9, 1.8, 1.9), parent="Castle", roughness=0.5)
    cir.e("Keep Roof", "cone", "#e8392f", (0, 5.6, 0), scale=(5, 2.4, 5), parent="Castle", roughness=0.5)
    cir.e("Castle Door", "cube", "#7a4a2a", (0, 1.0, 2.02), scale=(1.2, 2.0, 0.1), parent="Castle")
    cir.e("Flag Pole", "cylinder", "#dddddd", (0, 8.0, 0), scale=(0.08, 2.4, 0.08), parent="Castle", metallic=0.9)
    cir.e("Flag", "cube", "#ffd23a", (0.6, 8.8, 0), scale=(1.1, 0.7, 0.05), parent="Castle")
    cir.e("Star", "sphere", "#000000", (0, 9.6, 0), scale=0.5, parent="Castle", emissive=[1, 0.85, 0.2, 4], unlit=True)
    for k in range(5):
        cir.e(f"Waterfall {k+1}", "cube", [0.45, 0.8, 1, 0.75], (6.2, -0.5 - k * 2.2, 1.2), scale=(1.6, 2.4, 0.4),
              emissive=[0.5, 0.85, 1, 0.9], roughness=0.1, tags=["water"])
    cir.e("Waterfall Pool", "cylinder", [0.55, 0.85, 1, 0.8], (6.0, 2.52, 1.2), scale=(1.8, 0.05, 1.8), emissive=[0.6, 0.9, 1, 0.3])
    for k, (x, z) in enumerate([(-8, 2), (8, -3)]):
        cir.e(f"Floater {k+1}", "cube", "#ffd23a", (x, 3.5, z), scale=(2.2, 0.4, 2.2), roughness=0.4, tags=["platform"])
        cir.e(f"Floater {k+1} Trim", "cube", "#e8a21a", (x, 3.25, z), scale=(2.3, 0.15, 2.3), roughness=0.4)
    cir.flush("Castle, waterfall, floating platforms")
    for k in range(34):
        a = r.uniform(0, 6.28)
        d = r.uniform(6, 34)
        c = f"Cloud {k+1}"
        cir.e(c, pos=(math.cos(a) * d, r.uniform(-12, -6), math.sin(a) * d), tags=["cloud"], vars={"speed": r.uniform(0.3, 0.8)})
        for j in range(3):
            cir.e(f"{c} Puff {j+1}", "sphere", "#ffffff", (j * 1.6 - 1.6, (j == 1) * 0.6, 0), scale=(2.6 + (j == 1), 1.8 + (j == 1), 2.2),
                  parent=c, roughness=1)
    cir.flush("Sea of clouds")

    # Hero
    cir.e("Hero", pos=(RING, 0, 0), tags=["player"], vars={"coins": 0})
    cir.e("Hero Body", "sphere", "#ffffff", (0, 0.65, 0), scale=(1.0, 1.05, 1.0), parent="Hero", roughness=0.35)
    cir.e("Hero Belly", "sphere", "#3a6fe0", (0, 0.45, 0), scale=(1.03, 0.6, 1.03), parent="Hero", roughness=0.4)
    cir.e("Hero Cap", "sphere", "#e8392f", (0, 1.05, 0), scale=(0.95, 0.55, 0.95), parent="Hero", roughness=0.4)
    cir.e("Hero Visor", "cube", "#e8392f", (0, 1.0, -0.42), rot=(-12, 0, 0), scale=(0.7, 0.08, 0.4), parent="Hero")
    for s in (-1, 1):
        cir.e(f"Hero Eye {s}", "sphere", "#1a1a2a", (s * 0.17, 0.8, -0.44), scale=(0.12, 0.2, 0.08), parent="Hero")
        cir.e(f"Hero Shine {s}", "sphere", "#ffffff", (s * 0.17 + 0.03, 0.85, -0.48), scale=0.04, parent="Hero")
        cir.e(f"Hero Foot {s}", "sphere", "#7a4a2a", (s * 0.25, 0.1, -0.05), scale=(0.32, 0.2, 0.45), parent="Hero")
    cir.flush("The hero")

    # --- Aurora -------------------------------------------------------------------------------
    aur.call("environment_update", skyTop="#2a78f0", skyHorizon="#9fd4ff", ground="#8fb0e0", ambient=0.55,
             sunAzimuth=35, sunElevation=50, sunColor="#fff2d6", sunIntensity=2.6, fogColor="#bfe0ff",
             fogDensity=0.0015, exposure=1.0, showGrid=False, bloomIntensity=0.35, bloomThreshold=1.1,
             saturation=1.18, contrast=1.04, vignette=0.2, tonemap="neutral", clouds=0.25)
    aur.flush("Sunny sky")

    # --- Stratus ------------------------------------------------------------------------------
    seg = 2 * math.pi / N
    stra.behave("Hero", "Platforming", "Run around the island ring and jump each gap in a big arc, landing on the next island.", f"""
behavior Platforming
  on tick
    let a = time * {W}
    let u = (a % {seg:.5f}) / {seg:.5f}
    let p = (u + 0.17) % 1
    let h = 0
    if p < 0.34 then
      h = sin(p / 0.34 * pi) * 2.6
    end
    self.position = (cos(a) * {RING}, h, sin(a) * {RING})
    self.rotation = (0, 0 - a * 57.3, 0)
    if p < 0.34 then
      self.scale = (0.9, 1.15, 0.9)
    else
      let b = abs(sin(time * 12)) * 0.07
      self.scale = (1 + b * 0.5, 1 - b, 1 + b * 0.5)
    end
  end
  on event "coin"
    self.coins = self.coins + 1
  end
end""")
    for k in range(N):
        for c in range(3):
            stra.behave(f"Coin {k+1}.{c+1}", "Collect", "Spin; when the hero touches it, pop and count, then come back later.", """
behavior Collect
  var away = 0
  on tick
    rotate self by (0, 0, 200 * dt)
    away = max(0, away - dt)
    let h = find("Hero")
    if away == 0 and distance(self, h) < 1.3 then
      emit "coin" to h
      away = 8
    end
    if away > 0 then
      self.scale = (0.01, 0.01, 0.01)
    else
      self.scale = (0.9, 0.9, 0.9)
    end
  end
end""")
    for k in range(2):
        stra.behave(f"Floater {k+1}", "Bob", "Float up and down.", f"""
on tick
  self.position.y = 3.5 + sin(time * 1.2 + {k * 2}) * 0.8
end""")
    for k in range(34):
        stra.behave(f"Cloud {k+1}", "Drift", "Drift through the sky below.", """
on tick
  move self by (self.speed * dt, 0, 0)
  if self.position.x > 40 then
    self.position.x = -40
  end
end""")
    stra.behave("Flag", "Wave", "Wave in the wind.", """
on tick
  self.rotation = (0, sin(time * 5) * 18, 0)
end""")
    stra.behave("Star", "Shine", "Pulse like a prize.", """
on tick
  let s = 0.5 + sin(time * 3) * 0.08
  self.scale = (s, s, s)
end""")
    stra.e("Game Camera", pos=(RING + 9, 7, 0), camera={"fov": 55, "primary": True})
    stra.flush("Platforming, coins, platforms, clouds")
    stra.behave("Game Camera", "Chase", "Chase the hero from outside the ring, looking in toward the castle.", f"""
on tick
  let h = find("Hero")
  let a = time * {W} - 0.35
  self.position = lerp(self.position, (cos(a) * {RING + 10}, 6.5, sin(a) * {RING + 10}), 0.08)
  look self at (h.position.x * 0.7, 1.5, h.position.z * 0.7)
end""")
    stra.flush("Camera")
    nim.call("viewport_multi", size=256)
    nim.call("scene_save", path="scenes/main.sky.json")


def shots():
    def aerial(i, n):
        t = i / (n - 1)
        a = math.radians(70 + 50 * t)
        return dict(eye=[math.cos(a) * 34, 20 - 4 * t, math.sin(a) * 34], target=[0, 0, 0])

    def castle(i, n):
        t = i / (n - 1)
        return dict(eye=[10 - 3 * t, 4 + t, 12 - 2 * t], target=[0, 6, 0])

    return [dict(name="aerial", frames=150, cam=aerial),
            dict(name="castle", frames=120, cam=castle),
            dict(name="play", frames=240, view="scene", warmup=30)]
