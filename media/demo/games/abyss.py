"""Abyss — underwater survival horror. Something down here has a light of its own."""
import math
from kit import rnd

META = dict(
    id="abyss", title="Abyss", genre="Underwater Horror", mood="dark",
    pitch="Three hundred meters down, something else has a light.",
    view=dict(eye=[0, 9, 22], target=[0, 4, -2]),
)


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Abyss", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])

    # --- Pixel: textures, kelp (with its own sway behavior), rocks ----------------------------------
    sand = pix.texture("sand", "seabed_sand", color1="#7a8a78", color2="#5e6e5c", color3="#4a5a4a", tiling=0.25)
    rock = pix.texture("rock", "reef_rock", color1="#3a4644", color2="#26302f", color3="#4a5a3a", tiling=0.8)
    marble = pix.texture("marble", "temple_marble", color1="#8a9890", color2="#6a7870", color3="#3a4a44", tiling=0.5)
    pix.e("Kelp")
    for k in range(5):
        pix.e(f"Kelp Blade {k+1}", "cylinder", "#2f6b3a", (math.sin(k) * 0.12, 0.7 + k * 1.3, math.cos(k) * 0.1),
              rot=(math.sin(k * 1.7) * 8, 0, math.cos(k * 1.3) * 8), scale=(0.12, 1.5, 0.12), parent="Kelp", roughness=0.7)
        pix.e(f"Kelp Leaf {k+1}", "sphere", "#3d8a45", (0.22, 1.0 + k * 1.3, 0), rot=(0, 0, 40),
              scale=(0.5, 0.12, 0.22), parent="Kelp", roughness=0.6)
    pix.flush("Kelp model")
    pix.behave("Kelp", "Sway", "Sway slowly in the current; every stalk on its own phase.", """
on tick
  self.rotation = (sin(time * 0.8 + self.id) * 6, 0, cos(time * 0.6 + self.id * 1.7) * 7)
end""")
    pix.flush("Kelp sway")
    pix.prefab("Kelp", "prefabs/kelp.prefab.json", "Swaying kelp stalk (has a Sway behavior)", ["underwater", "plant"])
    pix.e("Rock")
    for k, (x, z, s) in enumerate([(0, 0, 1.3), (0.9, 0.3, 0.8), (-0.7, -0.4, 0.7)]):
        pix.e(f"Rock Lump {k+1}", "sphere", pos=(x, s * 0.25, z), scale=(s * 1.4, s * 0.8, s), parent="Rock", material=rock)
    pix.flush("Rock model")
    pix.prefab("Rock", "prefabs/rock.prefab.json", "Seabed rock cluster", ["underwater", "rock"])

    # --- Cirro: seabed, ruins, creatures ------------------------------------------------------
    cir.e("Seabed", "plane", pos=(0, 0, 0), scale=(300, 1, 300), material=sand, tags=["terrain"])
    for k, (x, z, sx, sz) in enumerate([(-8, -6, 9, 6), (9, -12, 12, 7), (2, 6, 8, 5), (-14, 8, 10, 8)]):
        cir.e(f"Dune {k+1}", "sphere", pos=(x, -0.6, z), scale=(sx, 1.8, sz), material=sand, tags=["terrain"])
    cir.e("Ruins")
    for k, (x, z, h, fallen) in enumerate([(-6, -12, 7, False), (-2, -13, 5.5, False), (2, -12.5, 3, False), (5, -10, 6, True)]):
        if fallen:
            cir.e(f"Column {k+1}", "cylinder", pos=(x, 0.6, z), rot=(0, 30, 88), scale=(1.1, h, 1.1), parent="Ruins", material=marble)
        else:
            cir.e(f"Column {k+1}", "cylinder", pos=(x, h / 2, z), scale=(1.1, h, 1.1), parent="Ruins", material=marble)
            cir.e(f"Column {k+1} Cap", "cube", pos=(x, h + 0.2, z), scale=(1.6, 0.4, 1.6), parent="Ruins", material=marble)
    cir.e("Lintel", "cube", pos=(-4, 7.4, -12.5), rot=(0, 0, 4), scale=(5.6, 0.6, 1.3), parent="Ruins", material=marble)
    cir.e("Idol", "sphere", "#0a0c0c", (-4, 1.2, -12.6), scale=(1.1, 1.6, 0.9), parent="Ruins", metallic=0.9, roughness=0.3)
    cir.e("Idol Eye", "sphere", "#000000", (-4, 1.6, -12.15), scale=(0.28, 0.12, 0.05), parent="Ruins",
          emissive=[0.2, 1, 0.6, 5], unlit=True)
    cir.flush("Seabed and ruins")
    cir.call("scatter", prefab="prefabs/kelp.prefab.json", count=46, center=[0, 0, -4], size=[44, 34], min_distance=1.6,
             scale=[0.6, 1.5], seed=21, surface="Seabed", group="Kelp Forest")
    cir.call("scatter", prefab="prefabs/rock.prefab.json", count=24, center=[0, 0, -2], size=[48, 36], min_distance=3,
             scale=[0.6, 1.8], seed=4, surface="Seabed", group="Rocks")

    # Anglerfish
    cir.e("Angler", pos=(6, 4, -6), tags=["monster"])
    cir.e("Angler Body", "sphere", "#33433f", (0, 0, 0), scale=(1.7, 1.4, 2.1), parent="Angler", roughness=0.45, metallic=0.2)
    cir.e("Angler Jaw", "sphere", "#141b1c", (0, -0.45, -0.75), rot=(18, 0, 0), scale=(1.6, 0.8, 1.4), parent="Angler", roughness=0.45)
    for k in range(9):
        x = -0.65 + k * 0.16
        cir.e(f"Angler Tooth {k+1}", "cone", "#e8e6d8", (x, -0.15, -1.45 + abs(x) * 0.5), rot=(180, 0, 0),
              scale=(0.07, 0.28 - abs(x) * 0.12, 0.07), parent="Angler", roughness=0.3)
    for s in (-1, 1):
        cir.e(f"Angler Eye {'L' if s < 0 else 'R'}", "sphere", "#c8d6b0", (s * 0.75, 0.35, -0.75), scale=0.2,
              parent="Angler", emissive=[0.7, 0.9, 0.5, 0.6], roughness=0.1)
        cir.e(f"Angler Fin {'L' if s < 0 else 'R'}", "cone", "#223033", (s * 0.95, -0.1, 0.4), rot=(0, 0, s * 70),
              scale=(0.5, 0.9, 0.2), parent="Angler")
    cir.e("Angler Tail", "cone", "#223033", (0, 0, 1.75), rot=(90, 0, 0), scale=(0.9, 1.0, 0.2), parent="Angler")
    cir.e("Angler Stalk", "cylinder", "#1b2426", (0, 1.35, -0.6), rot=(-35, 0, 0), scale=(0.06, 1.4, 0.06), parent="Angler")
    cir.e("Angler Lure", "sphere", "#000000", (0, 1.95, -1.15), scale=0.22, parent="Angler", emissive=[0.55, 1, 0.75, 7], unlit=True)
    cir.flush("Anglerfish")

    # Jellyfish
    r = rnd(8)
    palette = [[1, 0.45, 0.85], [0.45, 0.85, 1], [0.7, 0.55, 1]]
    for k in range(9):
        c = palette[k % 3]
        name = f"Jelly {k+1}"
        cir.e(name, pos=(r.uniform(-12, 10), r.uniform(3, 12), r.uniform(-12, 6)), tags=["jelly"])
        cir.e(f"{name} Bell", "sphere", c + [0.42], (0, 0, 0), scale=(0.8, 0.55, 0.8), parent=name, emissive=c + [1.6], roughness=0.2)
        cir.e(f"{name} Core", "sphere", "#000000", (0, -0.05, 0), scale=0.28, parent=name, emissive=c + [4], unlit=True)
        for t in range(6):
            a = t * math.pi / 3
            cir.e(f"{name} Tentacle {t+1}", "cylinder", c + [0.35], (math.cos(a) * 0.28, -0.85, math.sin(a) * 0.28),
                  scale=(0.025, 1.4 + (t % 2) * 0.5, 0.025), parent=name, emissive=c + [1.2])
    cir.flush("Jellyfish")

    # Submarine
    cir.e("Submarine", pos=(-16, 6.5, 3), tags=["player"])
    cir.e("Sub Hull", "capsule", "#f0b92c", (0, 0, 0), rot=(0, 0, 90), scale=(2.6, 4.6, 2.6), parent="Submarine", metallic=0.1, roughness=0.45)
    cir.e("Sub Tower", "cube", "#d9a521", (-0.2, 0.95, 0), scale=(1.0, 0.7, 0.55), parent="Submarine", metallic=0.1, roughness=0.45)
    for k in range(3):
        cir.e(f"Sub Porthole {k+1}", "cylinder", "#000000", (-0.8 + k * 0.8, 0.1, 0.64), rot=(90, 0, 0), scale=(0.32, 0.04, 0.32),
              parent="Submarine", emissive=[1, 0.75, 0.35, 2.5], unlit=True)
    cir.e("Sub Lamp", "sphere", "#000000", (2.25, 0.1, 0), scale=(0.15, 0.3, 0.3), parent="Submarine", emissive=[1, 0.95, 0.8, 6], unlit=True)
    cir.e("Propeller", pos=(-2.4, 0, 0), parent="Submarine")
    for k in range(3):
        cir.e(f"Propeller Blade {k+1}", "cube", "#8a7a50", (0, 0, 0), rot=(k * 60, 0, 0), scale=(0.05, 0.9, 0.16), parent="Propeller", metallic=0.9)
    cir.flush("Submarine")

    # Fish school + bubbles
    for k in range(18):
        cir.e(f"Fish {k+1}", "capsule", "#9fb4b8", (0, 5, 0), rot=(0, 0, 90), scale=(0.14, 0.3, 0.1), metallic=0.9, roughness=0.25,
              tags=["fish"], vars={"r": 3 + (k % 5) * 0.5, "h": 7 + (k % 4) * 0.4, "speed": 0.7 + (k % 3) * 0.08, "phase": k * 0.35})
    for k in range(28):
        cir.e(f"Bubble {k+1}", "sphere", [0.8, 0.95, 1, 0.3], (r.uniform(-10, 10), r.uniform(0, 14), r.uniform(-12, 6)),
              scale=r.uniform(0.06, 0.16), emissive=[0.6, 0.9, 1, 0.4], roughness=0.05, tags=["bubble"])
    cir.flush("Fish and bubbles")

    # --- Aurora: the last light of the surface ----------------------------------------------------
    aur.call("environment_update", skyTop="#0a5a6e", skyHorizon="#0b3d49", ground="#01090c", ambient=0.42,
             sunAzimuth=20, sunElevation=78, sunColor="#7fe0e8", sunIntensity=1.3, fogColor="#0b3d49",
             fogDensity=0.055, exposure=1.35, showGrid=False, bloomIntensity=0.7, bloomThreshold=0.9,
             saturation=1.1, contrast=1.1, vignette=0.55, ao=1.0, aoRadius=0.7)
    for k, (x, z, tilt) in enumerate([(-8, -4, 8), (-1, -9, -6), (5, 2, 10), (11, -8, -10), (-15, 5, 5), (2, -18, 4)]):
        aur.e(f"Light Shaft {k+1}", "cone", [0.6, 0.95, 1, 0.07], (x, 13, z), rot=(180 + tilt, 0, tilt * 0.6),
              scale=(3.2, 26, 3.2), emissive=[0.45, 0.85, 0.95, 0.35], roughness=1, tags=["godray"])
    aur.light("Lure Light", (0, 1.95, -1.15), "#7dffc8", 4.5, 9, parent="Angler")
    aur.light("Sub Spotlight", (2.3, 0.1, 0), "#fff1d6", 9, 22, kind="spot", rot=(-12, -90, 0), spot=22, parent="Submarine")
    aur.light("Sub Glow", (0, 0.4, 0.9), "#ffbe73", 1.2, 4, parent="Submarine")
    for k in (1, 4, 7):
        aur.light(f"Jelly {k} Light", (0, 0, 0), "#e48bff" if k == 1 else "#7fd6ff" if k == 4 else "#b49bff", 1.8, 6, parent=f"Jelly {k}")
    aur.light("Idol Light", (-4, 1.6, -11.6), "#3dff9a", 1.4, 5)
    aur.flush("Light shafts, lure, sub spotlight")

    # --- Stratus: life ------------------------------------------------------------------------
    stra.behave("Angler", "Stalk", "Circle the ruins slowly, as if it has all the time in the world.", """
behavior Stalk
  var home = (0, 0, 0)
  on start
    home = self.position
  end
  on tick
    let a = time * 0.16
    self.position = home + (cos(a) * 6.5, sin(time * 0.7) * 0.6, sin(a) * 4.5)
    self.rotation = (sin(time * 0.9) * 4, 0 - a * 57.3 + 180, sin(time * 0.7) * 5)
  end
end""")
    stra.behave("Angler Lure", "Dangle", "The lure pulses like a heartbeat.", """
on tick
  let p = max(0, sin(time * 2.4))
  self.scale = (0.2 + p * 0.06, 0.2 + p * 0.06, 0.2 + p * 0.06)
end""")
    stra.behave("Lure Light", "Pulse", "Pulse with the lure.", """
on tick
  self.light.intensity = 2.6 + max(0, sin(time * 2.4)) * 2.2
end""")
    for k in range(9):
        stra.behave(f"Jelly {k+1}", "Drift", "Rise in soft pulses, then sink back down; wrap around.", """
behavior Drift
  var phase = 0
  on start
    phase = self.id * 0.7
  end
  on tick
    let p = max(0, sin(time * 1.6 + phase))
    move self by (sin(time * 0.3 + phase) * 0.15 * dt, (p * 0.9 - 0.18) * dt, 0)
    self.scale = (1 - p * 0.12, 1 + p * 0.18, 1 - p * 0.12)
    if self.position.y > 14 then
      self.position.y = 1.5
    end
  end
end""")
    stra.behave("Submarine", "Cruise", "Cruise slowly across the trench, rocking a little, sweeping its spotlight.", """
behavior Cruise
  on tick
    move self by (0.75 * dt, sin(time * 0.8) * 0.12 * dt, 0)
    self.rotation = (sin(time * 0.5) * 6, sin(time * 0.35) * 12, sin(time * 0.9) * 2)
    if self.position.x > 22 then
      self.position.x = -22
    end
  end
end""")
    stra.behave("Propeller", "Spin", "Spin.", """
on tick
  rotate self by (420 * dt, 0, 0)
end""")
    for k in range(18):
        stra.behave(f"Fish {k+1}", "School", "Swim with the school in a tight loop around the columns.", """
on tick
  let a = time * self.speed + self.phase
  self.position = (-4 + cos(a) * self.r, self.h + sin(a * 2) * 0.3, -9 + sin(a) * self.r * 0.7)
  self.rotation = (0, 0 - a * 57.3, 90)
end""")
    for k in range(28):
        stra.behave(f"Bubble {k+1}", "Rise", "Rise, wobbling, then start again from the seabed.", """
on tick
  move self by (sin(time * 3 + self.id) * 0.2 * dt, 1.1 * dt, 0)
  if self.position.y > 15 then
    self.position.y = 0.2
  end
end""")
    stra.e("Game Camera", pos=(-16, 8, 12), camera={"fov": 55, "primary": True})
    stra.flush("Behaviors")
    stra.behave("Game Camera", "Follow", "Trail the submarine from behind and a little above.", """
on tick
  let s = find("Submarine")
  if exists(s) then
    self.position = lerp(self.position, s.position + (-5.5, 2.2, 7.5), 0.05)
    look self at s.position + (5, -0.6, -3)
  end
end""")
    stra.flush("Camera")
    nim.call("viewport_multi", size=256)
    nim.call("scene_save", path="scenes/main.sky.json")


def shots():
    def descent(i, n):
        t = i / (n - 1)
        e = t * t * (3 - 2 * t)
        return dict(eye=[2 - 4 * e, 15 - 7 * e, 24 - 8 * e], target=[0, 4 - e, -6])

    def angler(i, n):
        t = i / (n - 1)
        return dict(eye=[14 - 4 * t, 3.5 + t, 6 - 2 * t], target=[5, 4.2, -6])

    def jellies(i, n):
        t = i / (n - 1)
        a = math.radians(200 + 40 * t)
        return dict(eye=[math.cos(a) * 9, 1.5, math.sin(a) * 9 - 3], target=[-2, 9, -3])

    return [dict(name="descent", frames=150, cam=descent),
            dict(name="angler", frames=150, cam=angler, warmup=200),
            dict(name="jellies", frames=120, cam=jellies),
            dict(name="play", frames=180, view="scene", warmup=30)]
