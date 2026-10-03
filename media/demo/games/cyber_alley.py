"""Cyber Alley — neo-noir detective story in a rain-soaked megacity alley."""
import math
from kit import rnd

META = dict(
    id="cyber_alley", title="Cyber Alley", genre="Neo-Noir Adventure", mood="neon",
    pitch="It always rains in the lower city. Someone's waiting under the sign.",
    view=dict(eye=[0, 6, 26], target=[0, 6, -10]),
)

NEON = [[1, 0.15, 0.6], [0.1, 0.9, 1], [1, 0.62, 0.1], [0.6, 0.3, 1], [0.2, 1, 0.5]]


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Cyber Alley", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])
    r = rnd(2049)

    concrete = pix.texture("tiles", "wet_pavement", color1="#24242a", color2="#1c1c22", color3="#101014", roughness=0.12, scale=12, tiling=0.5)
    facade = pix.texture("bricks", "megablock", color1="#5a5664", color2="#4a4654", color3="#24222a", roughness=0.7, scale=10, tiling=0.25)
    metal = pix.texture("metal_brushed", "panel_metal", color1="#3a3e46", color2="#2a2e34", roughness=0.4, tiling=0.5)
    glow = pix.texture("glow", "soft_glow", size=128, glow=1.0)
    pix.material("materials/umbrella.mat.json", color="#14141a", roughness=0.3, clearcoat=0.8, rim=0.8)

    # --- Cirro: the street ---------------------------------------------------------------------
    cir.e("Street", "plane", pos=(0, 0, -20), scale=(200, 1, 200), material=concrete, tags=["terrain"])
    for s in (-1, 1):
        cir.e(f"Sidewalk {s}", "cube", pos=(s * 4.6, 0.12, -20), scale=(2.4, 0.24, 120), material=metal)
    for k, (x, z) in enumerate([(-1.2, 8), (1.8, 2), (-0.6, -6), (1.2, -15)]):
        cir.e(f"Puddle {k+1}", "cylinder", [0.06, 0.06, 0.09, 1], (x, 0.012, z), scale=(r.uniform(1.6, 2.6), 0.01, r.uniform(1.2, 2.2)),
              metallic=0.2, roughness=0.02)
    cir.flush("Wet street, sidewalks, puddles")
    n = 0
    for side in (-1, 1):
        z = 14.0
        while z > -70:
            depth = r.uniform(7, 12)
            h = r.uniform(18, 42)
            b = f"Block {n+1}"
            n += 1
            x = side * (6 + 5)
            cir.e(b, "cube", pos=(x, h / 2, z - depth / 2), scale=(10, h, depth - 0.4), material=facade, tags=["building"])
            # Lit windows on the street-facing facade
            face_x = side * 6.0 - side * 0.02
            for row in range(int(h / 2.2)):
                for col in range(int(depth / 1.8)):
                    if r.random() > 0.38:
                        continue
                    c = r.choice([[1, 0.8, 0.5], [0.5, 0.85, 1], [1, 0.4, 0.75], [0.9, 0.9, 1]])
                    cir.e(f"{b} Window {row}.{col}", "quad", "#000000",
                          (face_x, 3.4 + row * 2.2, z - 0.9 - col * 1.8), rot=(0, -side * 90, 0), scale=(1.0, 1.2, 1),
                          emissive=c + [r.uniform(0.35, 0.9)], unlit=True, tags=["window"])
            z -= depth
    cir.flush("Megablocks and windows")

    # Neon signs
    signs = []
    for k in range(14):
        side = -1 if k % 2 == 0 else 1
        z = 10 - k * 5.5 + r.uniform(-1, 1)
        y = r.uniform(4, 13)
        c = NEON[k % len(NEON)]
        name = f"Neon Sign {k+1}"
        signs.append(name)
        vertical = r.random() < 0.6
        sx, sy = (0.5, r.uniform(3, 6)) if vertical else (r.uniform(2.5, 4), 0.6)
        cir.e(name, pos=(side * 5.6, y, z), tags=["neon"])
        cir.e(f"{name} Panel", "cube", "#0a0a0e", (0, 0, 0), scale=(0.25, sy + 0.3, sx + 0.3) if vertical else (0.25, sy + 0.3, sx + 0.3),
              parent=name, roughness=0.4)
        cir.e(f"{name} Tube", "cube", "#000000", (-side * 0.14, 0, 0), scale=(0.08, sy, sx) if vertical else (0.08, sy, sx),
              parent=name, emissive=c + [4.5], unlit=True)
        # Wet-street trick: a soft colored streak where the sign would reflect on the asphalt.
        cir.e(f"{name} Reflection", "plane", c + [0.99], (side * 3.6, 0.012, z), scale=(sx * 0.6 + 1.2, 1, 9),
              texture="textures/soft_glow_albedo.png", emissiveMap="textures/soft_glow_albedo.png", emissive=c + [0.8],
              unlit=True, castShadows=False, tags=["reflection"])
        for g in range(3 if vertical else 1):
            cir.e(f"{name} Glyph {g+1}", "cube", "#000000", (-side * 0.2, (g - 1) * sy / 3.2 if vertical else 0, 0),
                  scale=(0.06, 0.35, 0.35), parent=name, emissive=[1, 1, 1, 3], unlit=True)
    cir.e("Holo Koi", pos=(0, 15, -18), tags=["hologram"])
    cir.e("Holo Koi Body", "capsule", [0.3, 0.9, 1, 0.35], (0, 0, 0), rot=(0, 0, 90), scale=(2.4, 6, 2.4), parent="Holo Koi",
          emissive=[0.2, 0.8, 1, 2.2], unlit=True)
    cir.e("Holo Koi Tail", "cone", [0.3, 0.9, 1, 0.35], (-3.6, 0, 0), rot=(0, 0, 90), scale=(1.8, 1.6, 0.3), parent="Holo Koi",
          emissive=[0.2, 0.8, 1, 2.2], unlit=True)
    cir.flush("Neon signs and a holographic koi")

    # Life: rain, steam, flying cars, a figure
    for k in range(170):
        cir.e(f"Rain {k+1}", "cube", [0.7, 0.8, 1, 0.35], (r.uniform(-5, 5), r.uniform(0, 16), r.uniform(-30, 16)),
              scale=(0.015, 0.7, 0.015), emissive=[0.5, 0.6, 0.9, 0.5], tags=["rain"], vars={"speed": r.uniform(16, 22)})
    for k in range(10):
        cir.e(f"Steam {k+1}", "sphere", [0.85, 0.85, 0.95, 0.12], (-4.6 + (k % 2) * 9.2, r.uniform(0.3, 3), -4 - (k // 2) * 7),
              scale=0.8, roughness=1, tags=["steam"], vars={"base": -4 - (k // 2) * 7, "phase": r.uniform(0, 4)})
    for k in range(8):
        car = f"Skycar {k+1}"
        lane = k % 3
        cir.e(car, pos=(r.uniform(-60, 60), 20 + lane * 5, -25 - lane * 8), tags=["skycar"],
              vars={"speed": (9 + lane * 3) * (1 if k % 2 else -1)})
        cir.e(f"{car} Body", "capsule", "#1a1c24", (0, 0, 0), rot=(0, 0, 90), scale=(1.6, 4.2, 1.4), parent=car, metallic=0.8, roughness=0.3, clearcoat=1)
        cir.e(f"{car} Head", "sphere", "#000000", (1.0 if k % 2 else -1.0, 0, 0), scale=0.3, parent=car, emissive=[1, 0.95, 0.85, 6], unlit=True)
        cir.e(f"{car} Tail", "sphere", "#000000", (-1.0 if k % 2 else 1.0, 0, 0), scale=0.25, parent=car, emissive=[1, 0.1, 0.15, 6], unlit=True)
        cir.e(f"{car} Under", "cube", "#000000", (0, -0.4, 0), scale=(1.6, 0.05, 0.5), parent=car, emissive=NEON[k % 5] + [3], unlit=True)
    cir.e("Detective", pos=(-1.2, 0, 4), rot=(0, 160, 0), tags=["player"])
    cir.e("Detective Coat", "capsule", "#3a3428", (0, 0.95, 0), scale=(0.7, 1.9, 0.55), parent="Detective", roughness=0.85)
    cir.e("Detective Head", "sphere", "#c69a7a", (0, 1.95, 0), scale=0.34, parent="Detective", subsurface=0.6)
    cir.e("Detective Hat", "cylinder", "#1a1714", (0, 2.12, 0), scale=(0.62, 0.05, 0.62), parent="Detective")
    cir.e("Detective Hat Top", "cylinder", "#1a1714", (0, 2.24, 0), scale=(0.36, 0.22, 0.36), parent="Detective")
    cir.e("Umbrella Shaft", "cylinder", "#111", (0.25, 1.9, 0), scale=(0.03, 1.4, 0.03), parent="Detective", metallic=1)
    cir.e("Umbrella", "cone", pos=(0.25, 2.65, 0), scale=(2.2, 0.6, 2.2), parent="Detective", material="materials/umbrella.mat.json")
    cir.flush("Rain, steam, skycars, the detective")

    # --- Aurora: the night ------------------------------------------------------------------------
    aur.call("environment_update", skyTop="#05030c", skyHorizon="#2a1238", ground="#0a0810", ambient=0.32,
             sunAzimuth=0, sunElevation=-20, sunIntensity=0, fogColor="#2a1838", fogDensity=0.03, fogHeight=0.06,
             exposure=1.25, showGrid=False, bloomIntensity=0.5, bloomThreshold=1.1, saturation=1.15, contrast=1.1,
             vignette=0.45, tonemap="agx", ao=1.0, reflections=1.6)
    for k, name in enumerate(signs[:11]):
        c = NEON[k % len(NEON)]
        aur.light(f"{name} Light", (0, 0, 0), "#" + "".join(f"{int(v * 255):02x}" for v in c), 4.5, 10, parent=name)
    aur.light("Holo Light", (0, 13, -18), "#40d8ff", 4, 16)
    aur.light("Doorway Light", (-4.8, 3, 3.5), "#ffb070", 3, 7)
    aur.flush("Neon spill light")

    # --- Stratus ---------------------------------------------------------------------------------
    for k, name in enumerate(signs):
        stra.behave(f"{name} Tube", "Buzz", "Neon hums; now and then it stutters.", """
behavior Buzz
  var base = (1, 1, 1)
  var dead = 0
  on start
    base = self.scale
  end
  on tick
    if chance(0.004) then
      dead = 0.25
    end
    dead = max(0, dead - dt)
    if dead > 0 and sin(time * 90) > 0 then
      self.scale = (0.01, 0.01, 0.01)
    else
      self.scale = base
    end
  end
end""")
    stra.behave("Holo Koi", "Swim", "The hologram swims in slow circles above the street, flickering.", """
on tick
  let a = time * 0.4
  self.position = (cos(a) * 3, 15 + sin(time * 0.8) * 0.6, -18 + sin(a) * 3)
  self.rotation = (0, 0 - a * 57.3 + 90, sin(time * 2) * 8)
end""")
    for k in range(170):
        stra.behave(f"Rain {k+1}", "Fall", "Fall hard, slanted by the wind; start again at the top.", """
on tick
  move self by (-0.8 * dt, 0 - self.speed * dt, 0)
  if self.position.y < 0 then
    self.position = (random(-5, 5), 16, random(-30, 16))
  end
end""")
    for k in range(10):
        stra.behave(f"Steam {k+1}", "Billow", "Rise from the grate, swell and fade, again and again.", """
on tick
  let t = (time * 0.5 + self.phase) % 3
  self.position = (self.position.x, 0.2 + t * 1.3, self.base)
  self.scale = (0.6 + t * 0.9, 0.6 + t * 0.9, 0.6 + t * 0.9)
  self.color = color(0.85, 0.85, 0.95, 0.14 * (1 - t / 3))
end""")
    for k in range(8):
        stra.behave(f"Skycar {k+1}", "Traffic", "Fly along the sky lane; loop back around the block.", """
on tick
  move self by (self.speed * dt, 0, 0)
  if self.position.x > 70 then
    self.position.x = -70
  elif self.position.x < -70 then
    self.position.x = 70
  end
end""")
    stra.behave("Detective", "Wait", "Wait under the sign, shifting weight now and then.", """
on tick
  self.rotation = (0, 160 + sin(time * 0.4) * 12, sin(time * 0.9) * 1.5)
end""")
    stra.e("Game Camera", pos=(1.5, 1.6, 12), camera={"fov": 50, "primary": True})
    stra.flush("Neon buzz, hologram, rain, steam, traffic")
    stra.behave("Game Camera", "Dolly", "A slow push down the alley at eye level.", """
on tick
  self.position = (1.4 + sin(time * 0.2) * 0.3, 1.7, 12 - (time % 30) * 0.12)
  look self at (0, 4.5, -25)
end""")
    stra.flush("Camera")
    nim.call("viewport_multi", size=256)
    nim.call("scene_save", path="scenes/main.sky.json")


def shots():
    def crane(i, n):
        t = i / (n - 1)
        e = t * t * (3 - 2 * t)
        return dict(eye=[0.5, 1.2 + 9 * e, 14 - 4 * e], target=[0, 6 + 4 * e, -30])

    def detective(i, n):
        t = i / (n - 1)
        a = math.radians(-30 + 40 * t)
        return dict(eye=[-1.2 + math.sin(a) * 4.2, 1.5, 4 + math.cos(a) * 4.2], target=[-1.2, 1.6, 4])

    def skyline(i, n):
        t = i / (n - 1)
        return dict(eye=[0, 22, 20 - 6 * t], target=[0, 14, -40])

    return [dict(name="crane", frames=165, cam=crane),
            dict(name="detective", frames=135, cam=detective),
            dict(name="skyline", frames=120, cam=skyline),
            dict(name="play", frames=150, view="scene")]
