"""Frostlight — a cozy arctic night: aurora, an igloo glow, and a penguin family on a stroll."""
import math
from kit import rnd

META = dict(
    id="frostlight", title="Frostlight", genre="Cozy Exploration", mood="cold",
    pitch="Under the aurora, the penguins are going home.",
    view=dict(eye=[10, 6, 18], target=[0, 2, -4]),
)


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Frostlight", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])
    r = rnd(1225)

    snow = pix.texture("noise", "snowfield", color1="#eef4ff", color2="#dbe6f8", color3="#c8d6f0", roughness=0.7, scale=6, bump=0.8, tiling=0.2)
    blocks = pix.texture("bricks", "snow_blocks", color1="#f2f6ff", color2="#e2eaf8", color3="#b8c8e4", roughness=0.6, scale=5, tiling=0.8)
    pix.material("materials/ice.mat.json", preset="ice", color="#9fd8ffcc")
    pix.material("materials/snow.mat.json", preset="snow")
    aurora_mat = pix.texture("curtain", "aurora_curtain", color1="#7affc8", color2="#9a6aff", color3="#ffffff", size=256, scale=4, glow=2.2)
    pix.e("Snowy Pine")
    pix.e("Snowy Pine Trunk", "cylinder", "#4a3426", (0, 0.6, 0), scale=(0.35, 1.2, 0.35), parent="Snowy Pine")
    for k, (y, s) in enumerate([(1.6, 2.6), (2.8, 2.0), (3.9, 1.4)]):
        pix.e(f"Snowy Pine Tier {k+1}", "cone", "#1d3a2e", (0, y, 0), scale=(s, 1.6, s), parent="Snowy Pine", roughness=0.85)
        pix.e(f"Snowy Pine Snow {k+1}", "cone", pos=(0, y + 0.45, 0), scale=(s * 0.72, 0.7, s * 0.72), parent="Snowy Pine",
              material="materials/snow.mat.json")
    pix.flush("Snowy pine")
    pix.prefab("Snowy Pine", "prefabs/snowy_pine.prefab.json", "Pine tree with snow-capped tiers", ["arctic", "nature"])

    # --- Cirro -----------------------------------------------------------------------------
    cir.e("Snowfield", "plane", pos=(0, 0, 0), scale=(600, 1, 600), material=snow, tags=["terrain"])
    for k in range(12):
        a = r.uniform(0, 6.28)
        d = r.uniform(8, 40)
        cir.e(f"Drift {k+1}", "sphere", pos=(math.cos(a) * d, -0.3, math.sin(a) * d - 6), scale=(r.uniform(4, 9), 1.6, r.uniform(3, 6)),
              material=snow, tags=["terrain"])
    cir.e("Igloo", pos=(-4, 0, -4))
    cir.e("Igloo Dome", "sphere", pos=(0, 0, 0), scale=(5.2, 4.2, 5.2), parent="Igloo", material=blocks)
    cir.e("Igloo Tunnel", "cylinder", pos=(0, 0.6, 2.6), rot=(90, 0, 0), scale=(1.8, 1.8, 1.6), parent="Igloo", material=blocks)
    cir.e("Igloo Door", "sphere", "#000000", (0, 0.7, 3.42), scale=(1.1, 1.2, 0.08), parent="Igloo", emissive=[1, 0.62, 0.28, 2.5], unlit=True)
    cir.e("Campfire", pos=(1.5, 0, 1.5))
    for k in range(3):
        cir.e(f"Fire Log {k+1}", "cylinder", "#3a2618", (0, 0.15, 0), rot=(90, k * 60, 0), scale=(0.18, 1.1, 0.18), parent="Campfire")
    for k in range(4):
        cir.e(f"Fire Flame {k+1}", "cone", "#ffb04a99", ((k - 1.5) * 0.12, 0.45, (k % 2) * 0.1), scale=(0.3, 0.7, 0.3),
              parent="Campfire", emissive=[1, 0.5, 0.12, 3.5], unlit=True, tags=["flame"])
    for k in range(10):
        a = k * 0.628
        cir.e(f"Fire Stone {k+1}", "sphere", "#5a5a64", (math.cos(a) * 0.75, 0.08, math.sin(a) * 0.75), scale=(0.28, 0.18, 0.24), parent="Campfire")
    for k in range(9):
        a = r.uniform(0, 6.28)
        d = r.uniform(7, 16)
        h = r.uniform(1.5, 4.5)
        cir.e(f"Ice Spike {k+1}", "cone", pos=(math.cos(a) * d + 4, h / 2, math.sin(a) * d - 8), rot=(r.uniform(-12, 12), 0, r.uniform(-12, 12)),
              scale=(r.uniform(0.6, 1.2), h, r.uniform(0.6, 1.2)), material="materials/ice.mat.json", tags=["ice"])
    cir.flush("Snowfield, igloo, campfire, ice")
    cir.call("scatter", prefab="prefabs/snowy_pine.prefab.json", count=40, center=[0, 0, -22], size=[90, 30], min_distance=3,
             scale=[0.8, 1.8], seed=11, surface="Snowfield", group="Pine Forest")

    # Aurora curtains
    for k in range(34):
        t = k / 33
        x = -70 + t * 140
        z = -70 - math.sin(t * 5) * 14
        c = [0.2 + 0.4 * t, 1 - 0.55 * t, 0.55 + 0.3 * t]
        cir.e(f"Aurora {k+1}", "quad", pos=(x, 34 + math.sin(t * 9) * 3, z), rot=(0, math.degrees(math.cos(t * 5) * 0.6), 0),
              scale=(7.5, 26, 1), material=aurora_mat, castShadows=False, tags=["aurora"], vars={"phase": t * 6.28})
    for k in range(130):
        cir.e(f"Snowflake {k+1}", "sphere", "#ffffff", (r.uniform(-14, 14), r.uniform(0, 12), r.uniform(-14, 12)), scale=r.uniform(0.04, 0.08),
              emissive=[0.8, 0.85, 1, 0.4], tags=["snow"], vars={"speed": r.uniform(0.6, 1.2)})
    cir.flush("Aurora and snowfall")

    # Penguin family
    for k, size in enumerate([1.0, 0.92, 0.6, 0.5, 0.45]):
        p = f"Penguin {k+1}"
        cir.e(p, pos=(6 - k * 1.2, 0, 3), tags=["penguin"], vars={"lag": k * 0.55, "size": size})
        cir.e(f"{p} Body", "capsule", "#14161c", (0, 0.55 * size, 0), scale=(0.95 * size, 1.1 * size, 0.85 * size), parent=p, roughness=0.5)
        cir.e(f"{p} Belly", "sphere", "#f2f2ea", (0, 0.5 * size, -0.12 * size), scale=(0.36 * size, 0.62 * size, 0.2 * size), parent=p)
        cir.e(f"{p} Beak", "cone", "#ff9a2a", (0, 0.9 * size, -0.25 * size), rot=(-90, 0, 0), scale=(0.1 * size, 0.18 * size, 0.08 * size), parent=p)
        for s in (-1, 1):
            cir.e(f"{p} Eye {s}", "sphere", "#ffffff", (s * 0.09 * size, 0.98 * size, -0.2 * size), scale=0.06 * size, parent=p)
            cir.e(f"{p} Foot {s}", "sphere", "#ff9a2a", (s * 0.12 * size, 0.03, -0.1 * size), scale=(0.14 * size, 0.05, 0.22 * size), parent=p)
            cir.e(f"{p} Flipper {s}", "sphere", "#14161c", (s * 0.27 * size, 0.55 * size, 0), rot=(0, 0, s * 20), scale=(0.08 * size, 0.42 * size, 0.2 * size),
                  parent=p)
    cir.e("Scarf", "torus", "#d93a3a", (0, 0.82, 0), scale=(0.62, 0.6, 0.62), parent="Penguin 1", roughness=0.9)
    cir.flush("Penguin family")

    # --- Aurora (the artist) -----------------------------------------------------------------
    aur.call("environment_update", skyTop="#030612", skyHorizon="#0e1e36", ground="#1a2232", ambient=0.4,
             sunAzimuth=40, sunElevation=18, sunColor="#9ab8ff", sunIntensity=0.8, sunSize=2.4, stars=1.0,
             fogColor="#1a2a44", fogDensity=0.012, fogHeight=0.1, exposure=1.3, showGrid=False, bloomIntensity=0.7,
             bloomThreshold=0.9, saturation=1.08, contrast=1.06, vignette=0.38, tonemap="agx", ao=1.0, temperature=-0.15)
    aur.light("Campfire Light", (1.5, 0.8, 1.5), "#ff9a48", 5, 10)
    aur.light("Igloo Light", (-4, 1.2, -1.2), "#ffb060", 3.2, 7)
    aur.light("Aurora Glow", (0, 30, -60), "#4affb0", 2.0, 120)
    aur.flush("Moonlight, firelight, aurora glow")

    # --- Stratus ------------------------------------------------------------------------------
    for k in range(34):
        stra.behave(f"Aurora {k+1}", "Ripple", "Ripple like a curtain of light, colors drifting.", """
behavior Ripple
  var base = (0, 0, 0)
  on start
    base = self.position
  end
  on tick
    let w = sin(time * 0.6 + self.phase)
    self.position = base + (0, w * 2.5, cos(time * 0.4 + self.phase) * 3)
    self.scale = (7.5, 24 + w * 6, 1)
  end
end""")
    for k in range(130):
        stra.behave(f"Snowflake {k+1}", "Fall", "Drift down in the still air.", """
on tick
  move self by (sin(time * 0.8 + self.id) * 0.2 * dt, 0 - self.speed * dt, 0)
  if self.position.y < 0 then
    self.position.y = 12
  end
end""")
    for k in range(5):
        stra.behave(f"Penguin {k+1}", "Waddle", "Follow the family in a loop around the camp, waddling side to side.", """
on tick
  let a = time * 0.28 - self.lag
  self.position = (1 + cos(a) * 7, 0, 0 + sin(a) * 5)
  self.rotation = (0, 0 - a * 57.3 - 180, sin(time * 7 + self.lag * 3) * 9)
end""")
    for k in range(4):
        stra.behave(f"Fire Flame {k+1}", "Dance", "Flicker.", """
on tick
  self.scale = (0.3, 0.6 + sin(time * 11 + self.id) * 0.15 + random() * 0.12, 0.3)
end""")
    stra.behave("Campfire Light", "Flicker", "Flicker like a real fire.", """
on tick
  self.light.intensity = 4.6 + sin(time * 8) * 0.5 + random() * 0.8
end""")
    stra.e("Game Camera", pos=(12, 4, 14), camera={"fov": 52, "primary": True})
    stra.flush("Aurora, snow, penguins, fire")
    stra.behave("Game Camera", "Follow", "Keep the penguin family in frame with the aurora behind them.", """
on tick
  let p = find("Penguin 3")
  self.position = lerp(self.position, p.position + (5, 3.2, 9), 0.03)
  look self at p.position + (-2, 4, -14)
end""")
    stra.flush("Camera")
    nim.call("viewport_multi", size=256)
    nim.call("scene_save", path="scenes/main.sky.json")


def shots():
    def sky(i, n):
        t = i / (n - 1)
        return dict(eye=[-6 + 8 * t, 1.6, 14], target=[0, 22, -60])

    def camp(i, n):
        t = i / (n - 1)
        a = math.radians(20 + 40 * t)
        return dict(eye=[1.5 + math.cos(a) * 9, 2.4, 1.5 + math.sin(a) * 9], target=[-1, 1.2, -1])

    def penguins(i, n):
        t = i / (n - 1)
        return dict(eye=[9 - 2 * t, 0.8, 9], target=[2, 0.8, 0])

    return [dict(name="sky", frames=150, cam=sky),
            dict(name="camp", frames=150, cam=camp),
            dict(name="penguins", frames=120, cam=penguins, warmup=200),
            dict(name="play", frames=150, view="scene")]
