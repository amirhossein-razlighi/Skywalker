"""Hollow Manor — horror / exploration. A lantern in the fog, a manor that should be empty."""
import math
from kit import rnd

META = dict(
    id="hollow_manor", title="Hollow Manor", genre="Horror · Exploration", mood="dark",
    pitch="A lantern in the fog, and a manor that should be empty.",
    view=dict(eye=[9, 7, 22], target=[0, 3, -8]),
)


def dead_tree(b, name, h=5.0, seed=1):
    r = rnd(seed)
    b.e(name)
    b.e(f"{name} Trunk", "cylinder", "#2b2622", (0, h / 2, 0), scale=(0.32, h, 0.32), parent=name, roughness=1)
    for k in range(5):
        y = h * (0.45 + 0.1 * k)
        yaw = r.uniform(0, 360)
        tilt = r.uniform(38, 62)
        L = r.uniform(1.2, 2.4) * (1.0 - 0.1 * k)
        ox, oz = math.sin(math.radians(yaw)) * L * 0.35, math.cos(math.radians(yaw)) * L * 0.35
        b.e(f"{name} Branch {k+1}", "cylinder", "#2b2622", (ox, y + L * 0.3, oz), rot=(tilt, yaw, 0),
            scale=(0.1, L, 0.1), parent=name, roughness=1)


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Hollow Manor", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])

    # --- Pixel: reusable pieces -------------------------------------------------------------
    stone = pix.texture("bricks", "manor_stone", color1="#4a4650", color2="#3a3740", color3="#1c1b20", scale=6, tiling=0.35)
    pix.call("material_update", path=stone, color="#c8c4d0")
    path_mat = pix.texture("cobblestone", "path_stones", color1="#6a665c", color2="#4a463e", color3="#24221e", tiling=0.6)
    ground_mat = pix.texture("grass", "dead_grass", color1="#3a4030", color2="#2a2e22", color3="#4a4434", tiling=0.12)
    grave_mat = pix.texture("rock", "grave_stone", color1="#7a7c80", color2="#5a5c60", color3="#3a4a34", tiling=1.2)
    pix.material("materials/window_lit.mat.json", color="#000000", emissive=[1.0, 0.58, 0.22, 1.6], unlit=True)
    pix.material("materials/window_dark.mat.json", color="#22304d", roughness=0.12, metallic=0.7)
    dead_tree(pix, "Dead Tree", seed=3)
    pix.flush("Dead tree model")
    pix.prefab("Dead Tree", "prefabs/dead_tree.prefab.json", "Leafless crooked tree", ["horror", "nature"])
    pix.e("Gravestone", rot=(6, 0, 3))
    pix.e("Gravestone Slab", "cube", pos=(0, 0.55, 0), scale=(0.75, 1.1, 0.16), parent="Gravestone", material=grave_mat)
    pix.e("Gravestone Top", "cylinder", pos=(0, 1.1, 0), rot=(90, 0, 0), scale=(0.75, 0.16, 0.75), parent="Gravestone", material=grave_mat)
    pix.e("Gravestone Moss", "cube", "#334a2c", (0, 0.06, 0.02), scale=(0.8, 0.12, 0.2), parent="Gravestone", roughness=1)
    pix.flush("Gravestone model")
    pix.prefab("Gravestone", "prefabs/gravestone.prefab.json", "Weathered gravestone", ["horror", "prop"])

    # --- Cirro: ground, path, manor ---------------------------------------------------------
    cir.e("Ground", "plane", pos=(0, 0, -10), scale=(420, 1, 420), material=ground_mat, tags=["terrain"])
    for k in range(9):
        z = 14 - k * 3.2
        cir.e(f"Path {k+1}", "cube", pos=(math.sin(k * 0.7) * 0.6, 0.02, z), rot=(0, math.sin(k) * 8, 0),
              scale=(2.4, 0.04, 3.4), material=path_mat, tags=["path"])
    for k, (y, a) in enumerate([(0.25, 0.10), (0.6, 0.07), (1.1, 0.045)]):
        cir.e(f"Ground Mist {k+1}", "plane", [0.62, 0.7, 0.86, a], (0, y, -6), scale=(90, 1, 70), roughness=1, tags=["mist"])
    cir.flush("Ground, path and mist")

    m = stone
    cir.e("Manor")
    cir.e("Manor Hall", "cube", pos=(0, 4.5, -20), scale=(14, 9, 8), parent="Manor", material=m)
    for s in (-1, 1):
        cir.e(f"Manor Roof {'L' if s < 0 else 'R'}", "cube", "#16151b", (0, 10.6, -20 + s * 2.3), rot=(s * 42, 0, 0),
              scale=(15, 0.35, 6.4), parent="Manor", roughness=0.8)
        x = s * 9
        cir.e(f"Tower {'W' if s < 0 else 'E'}", "cylinder", pos=(x, 7, -18), scale=(4.6, 14, 4.6), parent="Manor", material=m)
        cir.e(f"Tower {'W' if s < 0 else 'E'} Roof", "cone", "#16151b", (x, 16.6, -18), scale=(5.4, 5.5, 5.4), parent="Manor", roughness=0.8)
        cir.e(f"Tower {'W' if s < 0 else 'E'} Spire", "cylinder", "#3a3a40", (x, 20, -18), scale=(0.08, 2, 0.08), parent="Manor", metallic=1)
    cir.e("Chimney", "cube", pos=(-4, 12.2, -21), scale=(1.2, 3.5, 1.2), parent="Manor", material=m)
    cir.e("Door", "cube", "#1d120c", (0, 1.6, -15.95), scale=(1.8, 3.2, 0.2), parent="Manor", roughness=0.7)
    cir.e("Door Arch", "cylinder", "#1d120c", (0, 3.2, -15.95), rot=(90, 0, 0), scale=(1.8, 0.2, 1.8), parent="Manor")
    for k in range(3):
        cir.e(f"Step {k+1}", "cube", "#3a3a40", (0, 0.15 + k * 0.25, -14.6 - k * 0.45), scale=(4.4 - k * 0.6, 0.3, 1.2), parent="Manor")
    lit = {(1, 0), (3, 1), (5, 1)}
    for row in range(2):
        for col in range(6):
            x = -5.5 + col * 2.2 + (0.7 if col >= 3 else 0)
            name = f"Window {row+1}.{col+1}"
            cir.e(name, "quad", pos=(x if col < 3 else x - 0.7, 3.4 + row * 3.4, -15.98), scale=(1.1, 1.8, 1),
                  parent="Manor", material="materials/window_lit.mat.json" if (col, row) in lit else "materials/window_dark.mat.json",
                  tags=["window"])
    cir.e("Tower Window", "quad", pos=(9, 11, -15.67), scale=(0.9, 1.5, 1), parent="Manor",
          material="materials/window_lit.mat.json", tags=["window"])
    cir.flush("Manor")

    # Graveyard fence + scattered stones and a dead forest (scatter = one undo step each)
    for k in range(16):
        x = -17 + k * 0.75
        cir.e(f"Fence {k+1}", "cylinder", "#111114", (x, 0.7, 6.5), scale=(0.06, 1.4, 0.06), metallic=0.9, roughness=0.5)
        cir.e(f"Fence Tip {k+1}", "cone", "#111114", (x, 1.5, 6.5), scale=(0.12, 0.25, 0.12), metallic=0.9)
    cir.e("Fence Rail", "cube", "#111114", (-11.4, 1.1, 6.5), scale=(11.4, 0.06, 0.06), metallic=0.9)
    cir.flush("Graveyard fence")
    cir.call("scatter", prefab="prefabs/gravestone.prefab.json", count=22, center=[-11, 0, 0], size=[10, 11],
             min_distance=1.5, yaw=[-20, 20], scale=[0.8, 1.25], seed=13, surface="Ground", group="Graveyard")
    cir.call("scatter", prefab="prefabs/dead_tree.prefab.json", count=14, center=[-20, 0, -6], size=[14, 34],
             min_distance=3.5, scale=[0.8, 1.5], seed=7, surface="Ground", group="West Woods")
    cir.call("scatter", prefab="prefabs/dead_tree.prefab.json", count=14, center=[17, 0, -2], size=[14, 34],
             min_distance=3.5, scale=[0.8, 1.6], seed=9, surface="Ground", group="East Woods")

    # Characters
    cir.e("Wanderer", pos=(0.2, 0, 11), tags=["player"])
    cir.e("Wanderer Coat", "capsule", "#2c2638", (0, 0.85, 0), scale=(0.62, 0.85, 0.5), parent="Wanderer", roughness=0.9)
    cir.e("Wanderer Hood", "sphere", "#231e2d", (0, 1.38, 0.02), scale=(0.42, 0.44, 0.42), parent="Wanderer", roughness=0.9)
    cir.e("Lantern", pos=(0.42, 0.95, -0.25), parent="Wanderer")
    cir.e("Lantern Cage", "cube", "#20180f", (0, 0, 0), scale=(0.14, 0.2, 0.14), parent="Lantern", metallic=0.8, roughness=0.4)
    cir.e("Lantern Flame", "sphere", "#000000", (0, 0, 0), scale=0.09, parent="Lantern", emissive=[1, 0.55, 0.18, 6], unlit=True)
    cir.e("Ghost", pos=(-11, 1.4, 0), tags=["ghost"])
    cir.e("Ghost Body", "sphere", "#dfefff80", (0, 0, 0), scale=(0.8, 1.1, 0.8), parent="Ghost", emissive=[0.45, 0.8, 1, 1.2], roughness=0.3)
    cir.e("Ghost Tail", "cone", "#dfefff59", (0, -0.95, 0), rot=(180, 0, 0), scale=(0.75, 1.0, 0.75), parent="Ghost", emissive=[0.45, 0.8, 1, 0.8])
    for s in (-1, 1):
        cir.e(f"Ghost Eye {'L' if s < 0 else 'R'}", "sphere", "#020204", (s * 0.18, 0.2, 0.36), scale=(0.13, 0.2, 0.08), parent="Ghost")
    r = rnd(5)
    for k in range(7):
        x = r.choice([-1, 1]) * r.uniform(9, 22)
        cir.e(f"Eyes {k+1}", pos=(x, r.uniform(0.9, 2.2), r.uniform(-22, 2)), rot=(0, 180 if x > 0 else 0, 0), tags=["eyes"])
        for s in (-1, 1):
            cir.e(f"Eyes {k+1} {'L' if s < 0 else 'R'}", "sphere", "#000000", (s * 0.11, 0, 0), scale=0.055,
                  parent=f"Eyes {k+1}", emissive=[1, 0.06, 0.02, 8], unlit=True)
    for k in range(5):
        cir.e(f"Bat {k+1}", pos=(9, 15, -18), tags=["bat"],
              vars={"cx": 9, "cz": -18, "r": 3.5 + k * 0.8, "h": 15 + (k % 3) * 1.4, "speed": 1.1 + 0.15 * k, "phase": k * 1.3})
        cir.e(f"Bat {k+1} Body", "sphere", "#050506", (0, 0, 0), scale=(0.16, 0.14, 0.3), parent=f"Bat {k+1}")
        for s in (-1, 1):
            cir.e(f"Bat {k+1} Wing {'L' if s < 0 else 'R'}", "cube", "#050506", (s * 0.28, 0, 0), scale=(0.5, 0.02, 0.22),
                  parent=f"Bat {k+1}", vars={"side": s})
    cir.flush("Wanderer, ghost, watchers, bats")

    # --- Aurora: moonlight, fog, a lantern ---------------------------------------------------
    aur.call("environment_update", skyTop="#05070f", skyHorizon="#1c2438", ground="#14181f", ambient=0.22,
             sunAzimuth=200, sunElevation=28, sunColor="#a8baff", sunIntensity=1.25, fogColor="#1a2236",
             fogDensity=0.03, fogHeight=0.25, stars=0.8, sunSize=2.2, ao=1.2, aoRadius=0.8, temperature=-0.25,
             exposure=1.35, showGrid=False, bloomIntensity=0.45, bloomThreshold=1.0,
             saturation=0.85, contrast=1.08, vignette=0.42)
    aur.light("Lantern Light", (0, 0, 0), "#ffae5c", 3.0, 9, parent="Lantern")
    aur.light("Ghost Glow", (0, 0, 0), "#8fd4ff", 1.6, 5.5, parent="Ghost")
    aur.light("Porch Light", (-2.4, 3.6, -15.2), "#ff9a48", 2.4, 7)
    aur.e("Porch Lamp", "sphere", "#000000", (-2.4, 3.6, -15.5), scale=0.22, emissive=[1, 0.6, 0.25, 5], unlit=True)
    aur.light("Window Spill", (1.0, 7, -13.5), "#ffa65a", 1.6, 9)
    aur.light("Lightning", (0, 30, 0), "#cfdcff", 0.0, kind="directional", rot=(-60, 25, 0))
    aur.flush("Moonlight, lantern, porch and window light")

    # --- Stratus: behaviors (intent + Wander) -------------------------------------------------
    stra.behave("Wanderer", "Walk", "Walk slowly up the path toward the manor door, a little unsteady.", """
behavior Walk
  var speed = 0.85
  on tick
    if self.position.z > -12 then
      move self by (sin(time * 0.8) * 0.18 * dt, 0, -speed * dt)
    end
    self.position.y = abs(sin(time * 4.4)) * 0.05
  end
end""")
    stra.behave("Lantern Light", "Flicker", "Flicker like a real flame.", """
on tick
  self.light.intensity = 2.7 + sin(time * 13) * 0.25 + random() * 0.6
end""")
    stra.behave("Lantern", "Swing", "Swing gently with each step.", """
on tick
  self.rotation = (sin(time * 4.4) * 7, 0, sin(time * 2.2) * 10)
end""")
    stra.behave("Ghost", "Haunt", "Drift in slow loops over the graveyard, bobbing, always turned toward its path.", """
behavior Haunt
  var home = (0, 0, 0)
  on start
    home = self.position
  end
  on tick
    let a = time * 0.32
    self.position = home + (cos(a) * 4.2, sin(time * 1.6) * 0.25, sin(a) * 3.4)
    self.rotation = (0, 0 - a * 57.3, sin(time * 2) * 6)
  end
end""")
    stra.behave("Ghost Body", "Shimmer", "Breathe in and out of visibility.", """
on tick
  self.color = color(0.87, 0.94, 1, 0.33 + sin(time * 2.6) * 0.14)
end""")
    for k in range(7):
        stra.behave(f"Eyes {k+1}", "Blink", "Watch from the trees; blink at random.", """
behavior Blink
  var shut = 0
  on tick
    if chance(0.008) then
      shut = 0.16
    end
    shut = max(0, shut - dt)
    if shut > 0 then
      self.scale = (1, 0.08, 1)
    else
      self.scale = (1, 1, 1)
    end
  end
end""")
    for k in range(5):
        stra.behave(f"Bat {k+1}", "Circle", "Circle the east tower, dipping and climbing.", """
on tick
  let a = time * self.speed + self.phase
  self.position = (self.cx + cos(a) * self.r, self.h + sin(a * 3) * 0.5, self.cz + sin(a) * self.r)
  self.rotation = (0, 0 - a * 57.3, sin(a * 3) * 15)
end""")
        for s in "LR":
            stra.behave(f"Bat {k+1} Wing {s}", "Flap", "Flap fast.", """
on tick
  self.rotation = (0, 0, sin(time * 22) * 50 * self.side)
end""")
    stra.behave("Lightning", "Storm", "Every few seconds there may be a lightning flash: a sharp double flicker.", """
behavior Storm
  var flash = 0
  on tick
    every 3.5 seconds
      if chance(0.55) then
        flash = 1
      end
    end
    self.light.intensity = flash * 5 * (0.55 + 0.45 * sin(time * 70))
    flash = max(0, flash - dt * 3)
  end
end""")
    stra.behave("Tower Window", "Candle", "Someone is up there with a candle.", """
on tick
  self.scale = (0.9, 1.5, 1)
  self.mesh.emissive = color(1, 0.55, 0.2, 0.6 + sin(time * 7) * 0.2 + random() * 0.25)
end""")
    stra.e("Game Camera", pos=(1.8, 2.6, 16), camera={"fov": 52, "primary": True})
    stra.flush("Behaviors: wanderer, lantern, ghost, eyes, bats, storm")
    stra.behave("Game Camera", "Follow", "Follow behind the wanderer's shoulder, looking up the path at the manor.", """
on tick
  let p = find("Wanderer")
  if exists(p) then
    self.position = lerp(self.position, p.position + (1.5, 2.3, 4.6), 0.06)
    look self at p.position + (0, 1.8, -9)
  end
end""")
    stra.flush("Camera")

    nim.call("viewport_multi", size=256)
    nim.call("scene_save", path="scenes/main.sky.json")


def shots():
    def establish(i, n):
        t = i / (n - 1)
        e = t * t * (3 - 2 * t)
        return dict(eye=[2 - 4 * e, 5.5 - 1.2 * e, 30 - 12 * e], target=[0, 5, -18])

    def graveyard(i, n):
        t = i / (n - 1)
        return dict(eye=[-19 + 6 * t, 1.6 + 0.6 * t, 9 - 2 * t], target=[-10, 1.4, -1])

    def woods(i, n):
        t = i / (n - 1)
        return dict(eye=[15 - 3 * t, 1.7, 7 - 5 * t], target=[2 - 2 * t, 4.5, -17])

    return [dict(name="establish", frames=165, cam=establish, warmup=60),
            dict(name="graveyard", frames=135, cam=graveyard),
            dict(name="woods", frames=120, cam=woods),
            dict(name="play", frames=180, view="scene", warmup=0)]
