"""Harvest Fair — a family carnival at golden hour."""
import math
from kit import rnd

META = dict(
    id="harvest_fair", title="Harvest Fair", genre="Family · Park Builder", mood="warm",
    pitch="Golden hour, cotton candy, one more ride.",
    view=dict(eye=[18, 9, 22], target=[-2, 4, -6]),
)

WHEEL = (0.0, 10.0, -14.0)
R = 7.0
N_CABINS = 12
SHIRTS = ["#d9534f", "#5bc0de", "#f0ad4e", "#5cb85c", "#9b59b6", "#f7f7f7", "#2c6fbb", "#e67e22"]


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Harvest Fair", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])
    r = rnd(42)

    # --- Pixel: props -------------------------------------------------------------------------
    grass = pix.texture("grass", "meadow_grass", color1="#7d9a3c", color2="#5f7a2c", color3="#a8a04a", tiling=0.3)
    dirt = pix.texture("dirt", "fair_path", color1="#c9a36b", color2="#a8834e", color3="#7a5a38", tiling=0.4)
    pix.material("materials/bulb.mat.json", color="#000000", emissive=[1, 0.78, 0.4, 4], unlit=True)
    pix.e("Pumpkin")
    pix.e("Pumpkin Body", "sphere", "#e8771e", (0, 0.28, 0), scale=(0.7, 0.5, 0.7), parent="Pumpkin", roughness=0.6)
    pix.e("Pumpkin Stem", "cylinder", "#4e6a2a", (0, 0.58, 0), scale=(0.06, 0.16, 0.06), parent="Pumpkin")
    pix.flush("Pumpkin")
    pix.prefab("Pumpkin", "prefabs/pumpkin.prefab.json", "Autumn pumpkin", ["autumn", "prop"])
    pix.e("Autumn Tree")
    pix.e("Autumn Tree Trunk", "cylinder", "#5a3d28", (0, 1.4, 0), scale=(0.35, 2.8, 0.35), parent="Autumn Tree")
    for k, (x, y, z, s, c) in enumerate([(0, 3.4, 0, 2.6, "#d9611f"), (0.9, 2.9, 0.3, 1.8, "#e89a2c"), (-0.8, 3.0, -0.4, 1.9, "#b8461c"), (0.1, 4.3, 0.1, 1.6, "#f0b23a")]):
        pix.e(f"Autumn Tree Leaves {k+1}", "sphere", c, (x, y, z), scale=s, parent="Autumn Tree", roughness=0.9)
    pix.flush("Autumn tree")
    pix.prefab("Autumn Tree", "prefabs/autumn_tree.prefab.json", "Round tree in fall colors", ["autumn", "nature"])
    pix.e("Stall")
    pix.e("Stall Counter", "cube", "#e9dcc0", (0, 0.55, 0), scale=(2.6, 1.1, 1.2), parent="Stall", roughness=0.7)
    for sx in (-1, 1):
        for sz in (-1, 1):
            pix.e(f"Stall Post {sx}{sz}", "cylinder", "#f4f0e8", (sx * 1.25, 1.4, sz * 0.55), scale=(0.08, 2.8, 0.08), parent="Stall")
    for k in range(7):
        pix.e(f"Stall Stripe {k+1}", "cube", "#d9433a" if k % 2 == 0 else "#fff6e8", (-1.35 + k * 0.45, 2.95, 0.05),
              rot=(14, 0, 0), scale=(0.45, 0.08, 1.7), parent="Stall", roughness=0.8)
    pix.e("Stall Sign", "cube", "#2c5f8a", (0, 2.4, 0.75), scale=(1.6, 0.4, 0.06), parent="Stall")
    pix.e("Stall Bulb L", "sphere", pos=(-1.1, 2.2, 0.7), scale=0.09, parent="Stall", material="materials/bulb.mat.json")
    pix.e("Stall Bulb R", "sphere", pos=(1.1, 2.2, 0.7), scale=0.09, parent="Stall", material="materials/bulb.mat.json")
    pix.flush("Food stall")
    pix.prefab("Stall", "prefabs/stall.prefab.json", "Striped carnival food stall", ["carnival", "building"])

    # --- Cirro: grounds and rides --------------------------------------------------------------
    cir.e("Meadow", "plane", pos=(0, 0, 0), scale=(2400, 1, 2400), material=grass, tags=["terrain"])
    cir.e("Midway", "cube", pos=(2, 0.02, 0), scale=(6, 0.04, 34), material=dirt)
    cir.e("Plaza", "cylinder", pos=(-12, 0.02, -2), scale=(12, 0.04, 12), material=dirt)
    cir.flush("Meadow and paths")

    # Ferris wheel
    wx, wy, wz = WHEEL
    for sx in (-1, 1):
        for sz in (-1, 1):
            cir.e(f"Wheel Leg {sx}{sz}", "cylinder", "#f2efe8", (wx + sx * 2.4, wy / 2, wz + sz * 1.4), rot=(sz * -8, 0, sx * 14),
                  scale=(0.3, wy + 0.6, 0.3), metallic=0.6, roughness=0.3)
    cir.e("Wheel", pos=WHEEL, tags=["ride"])
    cir.e("Wheel Hub", "cylinder", "#d9433a", (0, 0, 0), rot=(90, 0, 0), scale=(1.1, 2.2, 1.1), parent="Wheel", metallic=0.4)
    for k in range(36):
        a = 2 * math.pi * k / 36
        for side in (-0.55, 0.55):
            cir.e(f"Rim {k+1}{'a' if side < 0 else 'b'}", "cube", "#f2efe8", (math.cos(a) * R, math.sin(a) * R, side),
                  rot=(0, 0, math.degrees(a) + 90), scale=(1.3, 0.16, 0.16), parent="Wheel", metallic=0.6, roughness=0.3)
    for k in range(12):
        a = 2 * math.pi * k / 12
        cir.e(f"Spoke {k+1}", "cube", "#f2efe8", (math.cos(a) * R / 2, math.sin(a) * R / 2, 0), rot=(0, 0, math.degrees(a)),
              scale=(R, 0.1, 0.1), parent="Wheel", metallic=0.6, roughness=0.3)
    for k in range(24):
        a = 2 * math.pi * k / 24 + 0.13
        cir.e(f"Rim Bulb {k+1}", "sphere", pos=(math.cos(a) * (R + 0.15), math.sin(a) * (R + 0.15), 0.6), scale=0.14,
              parent="Wheel", material="materials/bulb.mat.json", tags=["bulb"])
    for k in range(N_CABINS):
        c = SHIRTS[k % len(SHIRTS)]
        cab = f"Cabin {k+1}"
        cir.e(cab, pos=(wx, wy, wz), vars={"k": k}, tags=["cabin"])
        cir.e(f"{cab} Car", "cube", c, (0, -0.75, 0), scale=(1.2, 0.9, 1.1), parent=cab, roughness=0.5)
        cir.e(f"{cab} Roof", "cube", "#fff6e8", (0, 0.05, 0), scale=(1.4, 0.12, 1.3), parent=cab, roughness=0.5)
        cir.e(f"{cab} Rod", "cylinder", "#d0ccc4", (0, -0.15, 0), scale=(0.05, 0.4, 0.05), parent=cab, metallic=0.8)
    cir.flush("Ferris wheel")

    # Carousel
    cir.e("Carousel", pos=(-12, 0, -2), tags=["ride"])
    cir.e("Carousel Base", "cylinder", "#e9dcc0", (0, 0.25, 0), scale=(8.4, 0.5, 8.4), parent="Carousel", roughness=0.6)
    cir.e("Carousel Spin", pos=(0, 0, 0), parent="Carousel")
    cir.e("Carousel Pole", "cylinder", "#d9a63a", (0, 2.3, 0), scale=(0.6, 4.2, 0.6), parent="Carousel Spin", metallic=0.8, roughness=0.25)
    cir.e("Carousel Roof", "cone", "#d9433a", (0, 5.1, 0), scale=(9.2, 1.9, 9.2), parent="Carousel Spin", roughness=0.6)
    cir.e("Carousel Trim", "cylinder", "#fff6e8", (0, 4.2, 0), scale=(9.0, 0.35, 9.0), parent="Carousel Spin", roughness=0.6)
    cir.e("Carousel Flag", "cone", "#f0b23a", (0, 6.4, 0), scale=(0.4, 0.8, 0.4), parent="Carousel Spin")
    for k in range(16):
        a = 2 * math.pi * k / 16
        cir.e(f"Carousel Bulb {k+1}", "sphere", pos=(math.cos(a) * 4.55, 4.2, math.sin(a) * 4.55), scale=0.13,
              parent="Carousel Spin", material="materials/bulb.mat.json", tags=["bulb"])
    for k in range(8):
        a = 2 * math.pi * k / 8
        h = f"Horse {k+1}"
        cir.e(h, pos=(math.cos(a) * 3.1, 1.3, math.sin(a) * 3.1), rot=(0, -math.degrees(a), 0), parent="Carousel Spin", tags=["horse"])
        cir.e(f"{h} Pole", "cylinder", "#d9a63a", (0, 1.4, 0), scale=(0.07, 3.2, 0.07), parent=h, metallic=0.9, roughness=0.2)
        cir.e(f"{h} Body", "capsule", SHIRTS[(k + 3) % 8], (0, 0, 0), rot=(90, 0, 0), scale=(0.9, 1.6, 0.9), parent=h, roughness=0.35)
        cir.e(f"{h} Head", "sphere", SHIRTS[(k + 3) % 8], (0, 0.45, 0.62), scale=(0.3, 0.42, 0.45), parent=h, roughness=0.35)
        cir.e(f"{h} Mane", "cube", "#fff6e8", (0, 0.5, 0.28), scale=(0.08, 0.3, 0.5), parent=h)
    cir.flush("Carousel")

    # Stalls, string lights, decor
    for k in range(4):
        cir.op("prefab_instantiate", prefab="prefabs/stall.prefab.json", position=[8.5, 0, 6 - k * 5.5], yaw=-90, name=f"Stall {k+1}")
    poles = [(-1.2, z) for z in (10, 3, -4, -10)] + [(5.2, z) for z in (10, 3, -4, -10)]
    for k, (x, z) in enumerate(poles):
        cir.e(f"Light Pole {k+1}", "cylinder", "#3a3330", (x, 2.2, z), scale=(0.12, 4.4, 0.12), metallic=0.5)
    spans = [(poles[i], poles[i + 1]) for i in (0, 1, 2)] + [(poles[i], poles[i + 1]) for i in (4, 5, 6)] + [(poles[i], poles[i + 4]) for i in range(4)]
    n = 0
    for (a, b) in spans:
        for j in range(1, 10):
            t = j / 10
            x = a[0] + (b[0] - a[0]) * t
            z = a[1] + (b[1] - a[1]) * t
            y = 4.3 - math.sin(math.pi * t) * 0.9
            n += 1
            cir.e(f"String Bulb {n}", "sphere", pos=(x, y, z), scale=0.11, material="materials/bulb.mat.json", tags=["bulb"])
    for k in range(8):
        x, z = 6.5 + r.uniform(-0.5, 0.5), -12.5 + r.uniform(-0.6, 0.6)
        b = f"Balloon {k+1}"
        cir.e(b, pos=(x, 3.2 + r.uniform(0, 1.2), z), tags=["balloon"])
        cir.e(f"{b} Skin", "sphere", SHIRTS[k], (0, 0, 0), scale=(0.55, 0.65, 0.55), parent=b, roughness=0.25)
        cir.e(f"{b} String", "cylinder", "#f4f0e8", (0, -1.2, 0), scale=(0.015, 1.9, 0.015), parent=b)
    cir.flush("Stalls, string lights, balloons")
    cir.call("scatter", prefab="prefabs/pumpkin.prefab.json", count=30, center=[-2, 0, 8], size=[30, 10], min_distance=1.2,
             scale=[0.7, 1.6], seed=3, surface="Meadow", group="Pumpkin Patch")
    cir.call("scatter", prefab="prefabs/autumn_tree.prefab.json", count=22, center=[0, 0, -27], size=[80, 20], min_distance=5,
             scale=[0.9, 1.5], seed=19, surface="Meadow", group="Orchard")
    cir.call("scatter", prefab="prefabs/autumn_tree.prefab.json", count=10, center=[-30, 0, 6], size=[16, 30], min_distance=5,
             scale=[0.9, 1.4], seed=23, surface="Meadow", group="West Grove")
    for k in range(6):
        cir.e(f"Hay Bale {k+1}", "cylinder", "#d9b45a", (-4 + k * 1.3 + r.uniform(-0.2, 0.2), 0.5, 14 + r.uniform(-0.4, 0.4)),
              rot=(90, r.uniform(-20, 20), 0), scale=(1.0, 1.2, 1.0), roughness=1)
    for k in range(14):
        p = f"Visitor {k+1}"
        cir.e(p, pos=(r.uniform(-1, 5), 0, r.uniform(-12, 12)), tags=["visitor"],
              vars={"tx": r.uniform(-1, 5), "tz": r.uniform(-12, 12), "speed": r.uniform(0.7, 1.2)})
        h = r.uniform(0.75, 1.15)
        cir.e(f"{p} Body", "capsule", SHIRTS[k % 8], (0, 0.55 * h, 0), scale=(0.55 * h, 1.1 * h, 0.45 * h), parent=p, roughness=0.8)
        cir.e(f"{p} Head", "sphere", r.choice(["#f1c9a5", "#c68e63", "#8d5a3b", "#e8b28f"]), (0, 1.22 * h, 0), scale=0.34 * h, parent=p)
    cir.flush("Hay bales and visitors")

    # --- Aurora: golden hour ---------------------------------------------------------------------
    aur.call("environment_update", skyMode="atmosphere", clouds=0.42, skyTop="#5a6fb0", skyHorizon="#ffb27a",
             ground="#6a4a34", ambient=0.5, sunAzimuth=245, sunElevation=7, sunColor="#ffb06a", sunIntensity=2.8,
             sunSize=1.6, fogColor="#e8a888", fogDensity=0.004, exposure=1.1, showGrid=False, bloomIntensity=0.6,
             bloomThreshold=1.0, saturation=1.12, contrast=1.05, vignette=0.3, tonemap="agx", ao=1.0)
    aur.light("Wheel Glow", (0, 10, -12.5), "#ffc27a", 3, 12)
    aur.light("Carousel Glow", (-12, 3.6, -2), "#ffc27a", 3.2, 9)
    for k in range(4):
        aur.light(f"Stall Light {k+1}", (7.6, 2.2, 6 - k * 5.5), "#ffcf8a", 1.8, 5)
    aur.light("Midway Light 1", (2, 4, 6), "#ffb870", 2.2, 9)
    aur.light("Midway Light 2", (2, 4, -6), "#ffb870", 2.2, 9)
    aur.flush("Golden hour and ride lights")

    # --- Stratus: rides and people ---------------------------------------------------------------
    stra.behave("Wheel", "Turn", "Turn slowly and steadily.", """
on tick
  rotate self by (0, 0, 9 * dt)
end""")
    for k in range(N_CABINS):
        stra.behave(f"Cabin {k+1}", "Hang", "Ride the rim of the wheel but always hang upright, swinging a little.", f"""
on tick
  let w = find("Wheel")
  let a = (w.rotation.z + self.k * 30) * pi / 180
  self.position = ({WHEEL[0]} + cos(a) * {R}, {WHEEL[1]} + sin(a) * {R}, {WHEEL[2]} + 0.9)
  self.rotation = (0, 0, sin(time * 1.3 + self.k) * 4)
end""")
    stra.behave("Carousel Spin", "Spin", "Go round, gently.", """
on tick
  rotate self by (0, 22 * dt, 0)
end""")
    for k in range(8):
        stra.behave(f"Horse {k+1}", "Gallop", "Rise and fall as the carousel turns, out of step with the neighbors.", f"""
on tick
  self.position.y = 1.3 + sin(time * 2.2 + {k * 0.8}) * 0.35
end""")
    for k in range(8):
        stra.behave(f"Balloon {k+1}", "Bob", "Bob on its string in the breeze.", """
behavior Bob
  var base = (0, 0, 0)
  on start
    base = self.position
  end
  on tick
    self.position = base + (sin(time * 0.9 + self.id) * 0.15, sin(time * 1.3 + self.id) * 0.2, 0)
    self.rotation = (sin(time + self.id) * 6, 0, cos(time * 0.8 + self.id) * 6)
  end
end""")
    for k in range(14):
        stra.behave(f"Visitor {k+1}", "Wander", "Stroll between stalls; pick a new spot when you arrive.", """
on tick
  let target = (self.tx, 0, self.tz)
  if distance(self.position, target) < 0.4 then
    self.tx = random(-1, 5)
    self.tz = random(-12, 12)
  else
    look self at target
    move self toward target at self.speed
  end
  self.position.y = abs(sin(time * 6 + self.id)) * 0.06
end""")
    for k in range(1, 100):
        pass
    stra.e("Game Camera", pos=(16, 6, 20), camera={"fov": 50, "primary": True})
    stra.flush("Rides, balloons, visitors")
    stra.behave("Game Camera", "Crane", "A slow crane move across the fair.", """
on tick
  self.position = (16 + sin(time * 0.12) * 4, 6.5 + sin(time * 0.2) * 1.5, 19)
  look self at (-3, 5, -7)
end""")
    stra.flush("Camera")
    nim.call("viewport_multi", size=256)
    nim.call("scene_save", path="scenes/main.sky.json")


def shots():
    def wheel(i, n):
        t = i / (n - 1)
        return dict(eye=[9 - 6 * t, 2.2 + 0.8 * t, -2 + t], target=[0, 10, -14])

    def carousel(i, n):
        t = i / (n - 1)
        a = math.radians(30 + 40 * t)
        return dict(eye=[-12 + math.cos(a) * 8.5, 2.6, -2 + math.sin(a) * 8.5], target=[-12, 2.4, -2])

    def aerial(i, n):
        t = i / (n - 1)
        e = t * t * (3 - 2 * t)
        return dict(eye=[24 - 6 * e, 16 - 4 * e, 26 - 4 * e], target=[-3, 3, -6])

    return [dict(name="wheel", frames=150, cam=wheel),
            dict(name="carousel", frames=135, cam=carousel),
            dict(name="aerial", frames=150, cam=aerial),
            dict(name="play", frames=150, view="scene")]
