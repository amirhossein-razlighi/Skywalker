"""Hearthside — a cozy family life-sim. Snow outside, a fire inside, everyone home."""
import math
from kit import rnd

META = dict(
    id="hearthside", title="Hearthside", genre="Cozy Life Sim", mood="warm",
    pitch="Snow outside. Fire inside. Everyone's home.",
    view=dict(eye=[3.2, 2.4, 3.6], target=[-1.5, 1.2, -1.2]),
)

WOOD = ["#9a7048", "#8a6240", "#a67a50"]


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Hearthside", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])
    r = rnd(11)

    log = pix.texture("wood", "log_wood", color1="#b8925f", color2="#8a6440", color3="#5a3c24", tiling=0.9)
    stone = pix.texture("rock", "hearth_stone", color1="#a8a49e", color2="#7a7670", color3="#4a4642", tiling=1.6)
    floor = pix.texture("planks", "floorboards", color1="#a87a4c", color2="#8a6038", color3="#3a2414", tiling=1.1)
    rug = pix.texture("fabric", "wool_rug", color1="#9a3a2e", color2="#7a2a22", color3="#d9b26a", tiling=1.5)
    pix.material("materials/flame.mat.json", color="#ff8a2a99", emissive=[1, 0.45, 0.08, 3.5], unlit=True)

    # --- Cirro: the room --------------------------------------------------------------------
    cir.e("Floor", "cube", pos=(0, -0.05, 0.2), scale=(10, 0.1, 8.8), material=floor, tags=["floor"])
    for k in range(10):
        y = 0.21 + k * 0.4
        if 1.3 < y < 3.2:  # window opening in the back wall
            cir.e(f"Back Log {k+1}a", "cylinder", pos=(-1.75, y, -4), rot=(0, 0, 90), scale=(0.42, 6.5, 0.42), material=log)
            cir.e(f"Back Log {k+1}b", "cylinder", pos=(4.25, y, -4), rot=(0, 0, 90), scale=(0.42, 1.5, 0.42), material=log)
        else:
            cir.e(f"Back Log {k+1}", "cylinder", pos=(0, y, -4), rot=(0, 0, 90), scale=(0.42, 10.4, 0.42), material=log)
        cir.e(f"Right Log {k+1}", "cylinder", pos=(5, y, 0), rot=(90, 0, 0), scale=(0.42, 8.4, 0.42), material=log)
        cir.e(f"Left Log {k+1}", "cylinder", pos=(-5, y, 0), rot=(90, 0, 0), scale=(0.42, 8.4, 0.42), material=log)
        cir.e(f"Front Log {k+1}", "cylinder", pos=(0, y, 4.4), rot=(0, 0, 90), scale=(0.42, 10.4, 0.42), material=log)
    for k in range(5):
        cir.e(f"Beam {k+1}", "cube", "#5a3a22", (0, 4.05, -3.2 + k * 1.6), scale=(10.4, 0.35, 0.35), roughness=0.8)
    cir.e("Ceiling", "cube", "#4a3020", (0, 4.3, 0.2), scale=(10.4, 0.1, 9.2), roughness=0.9)
    cir.flush("Cabin walls, floor, beams")

    # Window + the night outside
    for name, pos, sc in [("Window Frame Top", (2.5, 3.2, -3.85), (2.3, 0.14, 0.2)), ("Window Frame Bottom", (2.5, 1.32, -3.85), (2.3, 0.14, 0.3)),
                          ("Window Frame L", (1.45, 2.26, -3.85), (0.14, 2.0, 0.2)), ("Window Frame R", (3.55, 2.26, -3.85), (0.14, 2.0, 0.2)),
                          ("Window Mullion", (2.5, 2.26, -3.85), (0.07, 2.0, 0.12)), ("Window Transom", (2.5, 2.26, -3.85), (2.1, 0.07, 0.12))]:
        cir.e(name, "cube", "#f1e6d2", pos, scale=sc, roughness=0.5)
    cir.e("Night Outside", "quad", "#000000", (2.5, 2.3, -5.4), scale=(5, 4, 1), emissive=[0.16, 0.24, 0.48, 1], unlit=True)
    cir.e("Moon Outside", "sphere", "#000000", (3.1, 2.9, -5.3), scale=0.35, emissive=[0.85, 0.9, 1, 2.2], unlit=True)
    cir.e("Snow Bank", "sphere", "#cfd9f2", (2.5, 0.9, -5.1), scale=(5, 1.2, 0.6), emissive=[0.35, 0.42, 0.7, 0.6])
    for k in range(4):
        cir.e(f"Pine Outside {k+1}", "cone", "#0e1a2c", (1.3 + k * 0.8, 1.6 + (k % 2) * 0.25, -5.25), scale=(0.5, 1.2 + (k % 2) * 0.4, 0.2))
    for k in range(26):
        cir.e(f"Snowflake {k+1}", "sphere", "#000000", (r.uniform(1.5, 3.5), r.uniform(1.3, 3.3), r.uniform(-4.9, -4.3)),
              scale=r.uniform(0.025, 0.05), emissive=[0.9, 0.95, 1, 1.4], unlit=True, tags=["snow"])
    cir.flush("Window and snowy night")

    # Fireplace on the left wall
    cir.e("Fireplace")
    for row in range(9):
        for col in range(5):
            y = 0.22 + row * 0.42
            z = -1.2 + col * 0.6 + (0.3 if row % 2 else 0)
            if row < 4 and 1 <= col <= 3 and not row % 2 or (row < 4 and 0.4 < z - (-1.2) < 2.2):
                continue  # firebox opening
            cir.e(f"Stone {row+1}.{col+1}", "cube", pos=(-4.55, y, z), scale=(0.55, 0.4, 0.56 + r.uniform(-0.04, 0.04)),
                  parent="Fireplace", material=stone)
    cir.e("Hearth", "cube", "#6b625a", (-4.2, 0.08, 0), scale=(1.4, 0.16, 3.2), parent="Fireplace", roughness=0.9)
    cir.e("Firebox", "cube", "#120a06", (-4.75, 0.85, 0), scale=(0.3, 1.5, 1.7), parent="Fireplace")
    cir.e("Mantel", "cube", "#5a3720", (-4.35, 1.9, 0), scale=(0.75, 0.16, 3.4), parent="Fireplace", roughness=0.6)
    for k in range(3):
        cir.e(f"Fire Log {k+1}", "cylinder", "#4a2a16", (-4.45, 0.3 + (k == 2) * 0.16, -0.25 + k * 0.25 - (k == 2) * 0.25),
              rot=(90 if k < 2 else 70, 20 * (k - 1), 0), scale=(0.18, 1.0, 0.18), parent="Fireplace", roughness=1)
    for k in range(5):
        cir.e(f"Flame {k+1}", "cone", pos=(-4.45, 0.62, -0.4 + k * 0.2), scale=(0.3, 0.55 + (k % 2) * 0.25, 0.3),
              parent="Fireplace", material="materials/flame.mat.json", tags=["flame"])
    for k in range(6):
        cir.e(f"Ember {k+1}", "sphere", "#000000", (-4.35 + r.uniform(-0.1, 0.15), 0.25, -0.5 + k * 0.2), scale=0.06,
              parent="Fireplace", emissive=[1, 0.25, 0.05, 4], unlit=True)
    for k, c in enumerate(["#b8312f", "#2f7a4a", "#b8312f"]):
        cir.e(f"Stocking {k+1}", "cube", c, (-4.0, 1.55, -0.9 + k * 0.9), scale=(0.08, 0.5, 0.26), parent="Fireplace", roughness=0.9)
        cir.e(f"Stocking {k+1} Cuff", "cube", "#f4efe6", (-4.0, 1.82, -0.9 + k * 0.9), scale=(0.1, 0.1, 0.28), parent="Fireplace", roughness=1)
    for k, z in enumerate([-1.3, 1.3]):
        cir.e(f"Candle {k+1}", "cylinder", "#f1e9d8", (-4.3, 2.12, z), scale=(0.09, 0.3, 0.09), parent="Fireplace")
        cir.e(f"Candle {k+1} Flame", "sphere", "#000000", (-4.3, 2.32, z), scale=(0.045, 0.08, 0.045), parent="Fireplace",
              emissive=[1, 0.7, 0.3, 5], unlit=True, tags=["candle"])
    cir.e("Garland", "torus", "#2f5a32", (-4.0, 1.95, 0), rot=(0, 0, 90), scale=(0.5, 3.0, 0.25), parent="Fireplace", roughness=1)
    cir.flush("Stone fireplace")

    # Furniture
    cir.e("Rug", "cylinder", pos=(-1.4, 0.02, 0.3), scale=(4.2, 0.03, 3.2), material=rug)
    cir.e("Rug Ring", "cylinder", "#d9b26a", (-1.4, 0.03, 0.3), scale=(3.4, 0.03, 2.5), roughness=1)
    cir.e("Rug Center", "cylinder", "#7a3a2e", (-1.4, 0.04, 0.3), scale=(3.0, 0.03, 2.1), roughness=1)
    cir.e("Sofa", pos=(1.5, 0, 0.6), rot=(0, 90, 0))
    for name, pos, sc, c in [("Seat", (0, 0.3, 0), (2.6, 0.45, 1.0), "#3f5f7a"), ("Back", (0, 0.75, -0.42), (2.6, 0.9, 0.25), "#3f5f7a"),
                             ("Arm L", (-1.35, 0.5, 0), (0.25, 0.75, 1.0), "#355069"), ("Arm R", (1.35, 0.5, 0), (0.25, 0.75, 1.0), "#355069"),
                             ("Cushion 1", (-0.6, 0.65, -0.2), (0.6, 0.45, 0.18), "#e4b75a"), ("Cushion 2", (0.6, 0.65, -0.2), (0.6, 0.45, 0.18), "#c8553d")]:
        cir.e(f"Sofa {name}", "cube", c, pos, scale=sc, parent="Sofa", roughness=0.95)
    cir.e("Blanket", "cube", "#c8553d", (0.75, 0.56, 0.1), rot=(0, 0, -8), scale=(0.9, 0.06, 0.95), parent="Sofa", roughness=1)
    cir.e("Coffee Table", "cube", "#6d4426", (-1.3, 0.42, 0.4), scale=(1.4, 0.08, 0.8), roughness=0.5)
    for s in ((-1, -1), (-1, 1), (1, -1), (1, 1)):
        cir.e(f"Table Leg {s}", "cube", "#5a3720", (-1.3 + s[0] * 0.62, 0.2, 0.4 + s[1] * 0.32), scale=(0.08, 0.4, 0.08))
    cir.e("Mug 1", "cylinder", "#e8e2d6", (-1.0, 0.54, 0.25), scale=(0.12, 0.16, 0.12), roughness=0.3)
    cir.e("Mug 2", "cylinder", "#c8553d", (-1.6, 0.54, 0.55), scale=(0.12, 0.16, 0.12), roughness=0.3)
    cir.e("Cocoa", "cylinder", "#4a2a18", (-1.0, 0.615, 0.25), scale=(0.1, 0.01, 0.1))
    cir.e("Board", "cube", "#d8c08f", (-1.45, 0.47, 0.35), rot=(0, 15, 0), scale=(0.5, 0.02, 0.5), roughness=0.6)
    cir.e("Floor Lamp", pos=(2.8, 0, 2.0))
    cir.e("Lamp Pole", "cylinder", "#2a2622", (0, 0.8, 0), scale=(0.05, 1.6, 0.05), parent="Floor Lamp", metallic=0.8)
    cir.e("Lamp Shade", "cone", [1, 0.85, 0.6, 0.85], (0, 1.75, 0), scale=(0.55, 0.45, 0.55), parent="Floor Lamp",
          emissive=[1, 0.7, 0.4, 1.1], roughness=1)
    cir.e("Bookshelf", pos=(4.6, 0, 1.6), rot=(0, -90, 0))
    cir.e("Shelf Frame", "cube", "#5a3720", (0, 1.2, 0), scale=(2.0, 2.4, 0.4), parent="Bookshelf", roughness=0.7)
    for row in range(4):
        x = -0.85
        while x < 0.8:
            w = r.uniform(0.08, 0.16)
            h = r.uniform(0.3, 0.42)
            cir.e(f"Book {row}.{int((x + 1) * 100)}", "cube", r.choice(["#b8312f", "#2f5a7a", "#d9b26a", "#3a6a3f", "#6d3a5a", "#e4d8bf"]),
                  (x + w / 2, 0.15 + row * 0.58 + h / 2, -0.24), scale=(w * 0.92, h, 0.3), parent="Bookshelf", roughness=0.8)
            x += w
    cir.flush("Rug, sofa, table, lamp, bookshelf")

    # Tree, presents, train
    cir.e("Tree", pos=(3.6, 0, -2.7))
    cir.e("Tree Trunk", "cylinder", "#4a2e1a", (0, 0.3, 0), scale=(0.2, 0.6, 0.2), parent="Tree")
    for k, (y, s, h) in enumerate([(0.95, 1.5, 1.2), (1.75, 1.15, 1.05), (2.45, 0.8, 0.95), (3.0, 0.45, 0.7)]):
        cir.e(f"Tree Tier {k+1}", "cone", "#1f5a34", (0, y, 0), scale=(s, h, s), parent="Tree", roughness=0.9)
    cir.e("Tree Star", "sphere", "#000000", (0, 3.45, 0), scale=0.16, parent="Tree", emissive=[1, 0.85, 0.35, 6], unlit=True)
    for k in range(30):
        t = k / 30
        a = t * 6 * math.pi
        rad = 0.7 * (1 - t) + 0.1
        y = 0.55 + t * 2.75
        c = [[1, 0.25, 0.2], [1, 0.8, 0.3], [0.3, 0.6, 1], [0.4, 1, 0.5]][k % 4]
        cir.e(f"Fairy Light {k+1}", "sphere", "#000000", (math.cos(a) * rad, y, math.sin(a) * rad), scale=0.05, parent="Tree",
              emissive=c + [5], unlit=True, tags=["fairy"])
    for k in range(10):
        a = k * 2.4
        cir.e(f"Bauble {k+1}", "sphere", ["#c8312f", "#d9b26a", "#3a6ab8"][k % 3], (math.cos(a) * 0.55, 0.8 + (k % 5) * 0.45, math.sin(a) * 0.55),
              scale=0.11, parent="Tree", metallic=0.9, roughness=0.15)
    for k, (dx, dz, c, s) in enumerate([(-0.9, 0.6, "#c8312f", 0.4), (0.5, 0.9, "#2f5a9a", 0.32), (-0.2, 1.2, "#d9b26a", 0.28), (0.9, 0.2, "#3a7a4a", 0.36)]):
        cir.e(f"Present {k+1}", "cube", c, (3.6 + dx, s / 2, -2.7 + dz), rot=(0, k * 23, 0), scale=s, roughness=0.6)
        cir.e(f"Present {k+1} Ribbon", "cube", "#f4efe6", (3.6 + dx, s / 2, -2.7 + dz), rot=(0, k * 23, 0), scale=(s * 1.02, s * 1.02, 0.06))
    cir.e("Train Track", "torus", "#3a3430", (3.6, 0.03, -2.7), scale=(2.6, 0.2, 2.6), metallic=0.6)
    cir.e("Toy Train", pos=(3.6, 0, -2.7))
    for k, c in enumerate(["#c8312f", "#2f5a9a", "#3a7a4a"]):
        a = -k * 0.42
        car = f"Train Car {k+1}"
        cir.e(car, "cube", c, (math.cos(a) * 1.3, 0.14, math.sin(a) * 1.3), rot=(0, -math.degrees(a), 0), scale=(0.22, 0.2, 0.36),
              parent="Toy Train", roughness=0.4)
    cir.e("Train Chimney", "cylinder", "#1a1a1a", (1.3, 0.3, 0), scale=(0.06, 0.14, 0.06), parent="Toy Train")
    cir.e("Train Lamp", "sphere", "#000000", (1.3, 0.17, -0.2), scale=0.04, parent="Toy Train", emissive=[1, 0.9, 0.5, 5], unlit=True)
    cir.flush("Tree, presents, toy train")

    # The family pets
    cir.e("Cat", pos=(-2.9, 0.06, 0.9), rot=(0, 40, 0), tags=["pet"])
    cir.e("Cat Body", "sphere", "#d9822b", (0, 0.18, 0), scale=(0.62, 0.34, 0.46), parent="Cat", roughness=1)
    cir.e("Cat Head", "sphere", "#d9822b", (0.3, 0.2, 0.18), scale=0.28, parent="Cat", roughness=1)
    for s in (-1, 1):
        cir.e(f"Cat Ear {s}", "cone", "#c46f20", (0.3 + s * 0.08, 0.36, 0.18), scale=(0.08, 0.12, 0.08), parent="Cat")
    cir.e("Cat Tail", "capsule", "#c46f20", (-0.1, 0.08, 0.3), rot=(0, 70, 90), scale=(0.1, 0.4, 0.1), parent="Cat")
    cir.e("Dog", pos=(-0.6, 0.06, 2.3), rot=(0, -20, 0), tags=["pet"])
    cir.e("Dog Body", "capsule", "#f0e2c4", (0, 0.24, 0), rot=(0, 0, 90), scale=(0.5, 0.65, 0.42), parent="Dog", roughness=1)
    cir.e("Dog Head", "sphere", "#f0e2c4", (0.62, 0.22, 0), scale=(0.34, 0.3, 0.32), parent="Dog", roughness=1)
    for s in (-1, 1):
        cir.e(f"Dog Ear {s}", "sphere", "#a8784a", (0.62, 0.24, s * 0.2), scale=(0.18, 0.06, 0.12), parent="Dog", roughness=1)
    cir.e("Dog Nose", "sphere", "#1a1210", (0.8, 0.22, 0), scale=0.05, parent="Dog")
    cir.flush("Cat and dog")

    # --- Aurora: firelight --------------------------------------------------------------------
    aur.call("environment_update", skyTop="#565a66", skyHorizon="#5a5450", ground="#2e2822", ambient=0.42,
             sunAzimuth=0, sunElevation=-20, sunIntensity=0, fogDensity=0, exposure=1.25, showGrid=False,
             bloomIntensity=0.5, bloomThreshold=0.95, saturation=1.0, contrast=1.06, vignette=0.4, ao=1.2, aoRadius=0.5)
    aur.light("Fire Light", (-4.0, 0.9, 0), "#ffb878", 4.6, 11)
    aur.light("Fire Glow", (-3.2, 0.4, 0.2), "#ff5a1e", 2.0, 5)
    aur.light("Lamp Light", (2.8, 1.65, 2.0), "#ffc27a", 2.6, 7)
    aur.light("Tree Glow", (3.4, 1.6, -2.2), "#ffd28a", 1.6, 4)
    aur.light("Candle Glow", (-4.1, 2.4, 0), "#ffb060", 1.0, 3.5)
    aur.light("Moonlight", (2.5, 2.5, -3.6), "#7f9cff", 1.4, 6, kind="spot", rot=(-25, 0, 0), spot=40)
    aur.flush("Fire, lamp, tree glow, moonlight through the window")

    # --- Stratus: the life of the room ----------------------------------------------------------
    for k in range(5):
        stra.behave(f"Flame {k+1}", "Dance", "Flames lick up and down, never the same twice.", """
on tick
  let s = 0.55 + sin(time * 9 + self.id * 1.7) * 0.12 + random() * 0.12
  self.scale = (0.3, s, 0.3)
end""")
    stra.behave("Fire Light", "Flicker", "The firelight breathes and flickers.", """
on tick
  self.light.intensity = 5 + sin(time * 7) * 0.6 + random() * 1.1
end""")
    for k in range(30):
        stra.behave(f"Fairy Light {k+1}", "Twinkle", "Twinkle softly, each on its own rhythm.", """
on tick
  let b = 0.6 + 0.4 * sin(time * 2.2 + self.id * 0.9)
  self.scale = (0.035 + b * 0.025, 0.035 + b * 0.025, 0.035 + b * 0.025)
end""")
    stra.behave("Toy Train", "Choo", "Go round and round the tree.", """
on tick
  rotate self by (0, -28 * dt, 0)
end""")
    stra.behave("Cat Body", "Breathe", "Sleep and breathe slowly.", """
on tick
  let b = sin(time * 1.6) * 0.03
  self.scale = (0.62, 0.34 + b, 0.46 + b)
end""")
    stra.behave("Dog Body", "Breathe", "Snore gently.", """
on tick
  let b = sin(time * 1.2) * 0.035
  self.scale = (0.5 + b, 0.65, 0.42 + b)
end""")
    for k in range(26):
        stra.behave(f"Snowflake {k+1}", "Fall", "Fall past the window, drifting, and start again at the top.", """
on tick
  move self by (sin(time + self.id) * 0.12 * dt, -0.32 * dt, 0)
  if self.position.y < 1.35 then
    self.position.y = 3.25
  end
end""")
    stra.e("Game Camera", pos=(3.6, 2.5, 3.8), camera={"fov": 58, "primary": True})
    stra.flush("Fire, twinkle, train, pets, snow")
    stra.behave("Game Camera", "Drift", "A slow, dreamy drift around the room.", """
on tick
  self.position = (3.4 + sin(time * 0.15) * 0.5, 2.35 + sin(time * 0.2) * 0.1, 3.6)
  look self at (-1.2, 1.1, -1)
end""")
    stra.flush("Camera")
    nim.call("viewport_multi", size=256)
    nim.call("scene_save", path="scenes/main.sky.json")


def shots():
    def fireside(i, n):
        t = i / (n - 1)
        e = t * t * (3 - 2 * t)
        return dict(eye=[2.4 - 1.3 * e, 1.6 - 0.35 * e, 3.2 - 1.0 * e], target=[-4.4, 0.9, -0.2])

    def tree(i, n):
        t = i / (n - 1)
        return dict(eye=[1.2 + 0.9 * t, 1.1, 0.4 - 0.8 * t], target=[3.6, 1.3, -2.7])

    def window(i, n):
        t = i / (n - 1)
        return dict(eye=[0.6 + 0.8 * t, 1.7, -1.0 - 0.6 * t], target=[2.5, 2.2, -4.6])

    return [dict(name="fireside", frames=165, cam=fireside),
            dict(name="tree", frames=135, cam=tree),
            dict(name="window", frames=120, cam=window),
            dict(name="play", frames=150, view="scene")]
