"""Toy Kart Rally — a dusk stadium kart race with floodlights and fireworks."""
import math
from kit import rnd

META = dict(
    id="toy_kart_rally", title="Toy Kart Rally", genre="Kart Racing", mood="dusk",
    pitch="Four karts, one oval, and a crowd that wants fireworks.",
    view=dict(eye=[0, 30, 40], target=[0, 0, 0]),
)

A, B = 26.0, 14.0  # oval half-axes (centerline)
KARTS = [("Comet", "#e8392f"), ("Bolt", "#2f7ae8"), ("Zest", "#f2c21a"), ("Fern", "#33b85a")]


def oval(t, da=0.0, db=0.0):
    return math.cos(t) * (A + da), math.sin(t) * (B + db)


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Toy Kart Rally", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])
    r = rnd(9)

    for name, c in [("Gold", [1, 0.75, 0.25]), ("Pink", [1, 0.3, 0.7]), ("Cyan", [0.3, 0.85, 1])]:
        fw = f"Firework {name}"
        pix.e(fw, vars={"t": 0})
        for k in range(14):
            th = k * 2.399
            y = 1 - 2 * (k + 0.5) / 14
            rr = math.sqrt(1 - y * y)
            pix.e(f"{fw} Spark {k+1}", "sphere", "#000000", (math.cos(th) * rr, y, math.sin(th) * rr), scale=0.12, parent=fw,
                  emissive=c + [7], unlit=True)
        pix.flush(f"{name} firework")
        pix.behave(fw, "Burst", "Burst outward, sparks drifting down, then gone.", """
on tick
  self.t = self.t + dt
  let s = 0.5 + sqrt(self.t) * 5
  self.scale = (s, s, s)
  move self by (0, -1.2 * dt, 0)
  if self.t > 1.6 then
    destroy self
  end
end""")
        pix.flush(f"{name} firework behavior")
        pix.prefab(fw, f"prefabs/firework_{name.lower()}.prefab.json", f"{name} firework burst", ["fx", "celebration"])

    asphalt = pix.texture("noise", "asphalt", color1="#2a2b30", color2="#3a3b42", color3="#1a1a1e", roughness=0.85, scale=24, bump=0.6, tiling=0.4)
    turf = pix.texture("grass", "infield_turf", color1="#3f8a3a", color2="#2f7030", color3="#5aa04a", tiling=0.4)
    pix.material("materials/kart_paint.mat.json", preset="car_paint")
    pix.material("materials/tire.mat.json", preset="rubber")

    # --- Cirro: stadium -----------------------------------------------------------------------
    cir.e("Ground", "plane", "#3c4048", (0, 0, 0), scale=(400, 1, 400), roughness=0.95, tags=["terrain"])
    cir.e("Infield", "cylinder", pos=(0, 0.02, 0), scale=(2 * A - 7, 0.04, 2 * B - 7), material=turf)
    segs = 56
    for k in range(segs):
        t0, t1 = 2 * math.pi * k / segs, 2 * math.pi * (k + 1) / segs
        x0, z0 = oval(t0)
        x1, z1 = oval(t1)
        L = math.hypot(x1 - x0, z1 - z0)
        yaw = -math.degrees(math.atan2(z1 - z0, x1 - x0))
        cir.e(f"Track {k+1}", "cube", pos=((x0 + x1) / 2, 0.05, (z0 + z1) / 2), rot=(0, yaw, 0), scale=(L + 0.35, 0.1, 7),
              material=asphalt, tags=["track"])
        for side, d in (("In", -3.75), ("Out", 3.75)):
            nx, nz = math.cos((t0 + t1) / 2) / A, math.sin((t0 + t1) / 2) / B
            nl = math.hypot(nx, nz)
            cx = (x0 + x1) / 2 + nx / nl * d
            cz = (z0 + z1) / 2 + nz / nl * d
            cir.e(f"Curb {side} {k+1}", "cube", "#e8392f" if k % 2 else "#f4f4f4", (cx, 0.12, cz), rot=(0, yaw, 0), scale=(L + 0.3, 0.12, 0.6),
                  roughness=0.6)
    cir.flush("Oval track and curbs")
    for row in range(2):
        for k in range(8):
            cir.e(f"Checker {row}.{k}", "cube", "#111111" if (row + k) % 2 else "#f4f4f4", (A - 3.06 + k * 0.875, 0.11, -0.45 + row * 0.9),
                  scale=(0.875, 0.02, 0.9))
    cir.e("Gantry", pos=(A, 0, 0))
    for s in (-1, 1):
        cir.e(f"Gantry Post {s}", "cube", "#d8d8e0", (s * 4.6, 2.6, 0), scale=(0.4, 5.2, 0.4), parent="Gantry", metallic=0.6)
    cir.e("Gantry Beam", "cube", "#1a1a22", (0, 5.2, 0), scale=(9.6, 0.9, 0.5), parent="Gantry")
    for k in range(5):
        cir.e(f"Start Light {k+1}", "sphere", "#000000", (-1.6 + k * 0.8, 5.2, 0.28), scale=0.3, parent="Gantry",
              emissive=[0.2, 1, 0.3, 5], unlit=True)
    for k in range(10):
        cir.e(f"Gantry Bulb {k+1}", "sphere", "#000000", (-4.2 + k * 0.93, 4.65, 0.27), scale=0.13, parent="Gantry",
              emissive=[1, 0.85, 0.5, 4], unlit=True)
    cir.flush("Finish line and gantry")

    # Grandstand + crowd along the far straight
    for row in range(5):
        cir.e(f"Stand Step {row+1}", "cube", "#5a5e6a", (0, 0.4 + row * 0.7, -B - 8 - row * 1.2), scale=(36, 0.8 + row * 1.4 * 0, 1.2), roughness=0.9)
    cir.e("Stand Back", "cube", "#40434d", (0, 3.0, -B - 14.6), scale=(37, 6.5, 0.6))
    cir.e("Stand Roof", "cube", "#2e3038", (0, 6.6, -B - 11.5), rot=(-8, 0, 0), scale=(38, 0.3, 8))
    shirts = ["#e8392f", "#2f7ae8", "#f2c21a", "#33b85a", "#ffffff", "#ff7ac8", "#ff8a2a"]
    n = 0
    for row in range(5):
        for k in range(22):
            n += 1
            x = -16.5 + k * 1.57 + r.uniform(-0.2, 0.2)
            cir.e(f"Fan {n}", "capsule", r.choice(shirts), (x, 1.15 + row * 0.7, -B - 8 - row * 1.2), scale=(0.7, 0.9, 0.6),
                  roughness=0.8, tags=["fan"], vars={"phase": r.uniform(0, 6.28)})
    cir.flush("Grandstand and crowd")
    for k, (x, z) in enumerate([(-A - 9, -B - 6), (A + 9, -B - 6), (-A - 9, B + 6), (A + 9, B + 6)]):
        cir.e(f"Flood Tower {k+1}", "cylinder", "#8a8e98", (x, 7, z), scale=(0.5, 14, 0.5), metallic=0.7)
        cir.e(f"Flood Panel {k+1}", "cube", "#000000", (x * 0.97, 14.2, z * 0.97), rot=(0, -math.degrees(math.atan2(z, x)) - 90, 0),
              scale=(3, 1.6, 0.3), emissive=[1, 0.95, 0.85, 6], unlit=True)
    for k in range(10):
        t = 2 * math.pi * (k + 0.5) / 10
        x, z = oval(t, 8, 7)
        for j in range(3):
            cir.e(f"Tire Stack {k+1}.{j+1}", "torus", "#141414", (x, 0.18 + j * 0.3, z), scale=(1.4, 1.6, 1.4), roughness=0.9)
    for k in range(24):
        a = r.uniform(0, 6.28)
        x, z = math.cos(a) * r.uniform(48, 70), math.sin(a) * r.uniform(36, 60)
        cir.e(f"Pine {k+1}", "cone", "#1f3a2a", (x, 4, z), scale=(4, 8 + r.uniform(0, 4), 4), roughness=0.9)
    cir.flush("Floodlights, tire stacks, pines")

    # Karts
    for k, (name, c) in enumerate(KARTS):
        kart = f"Kart {name}"
        lane = (k % 2) * 2.4 - 1.2
        x, z = oval(-0.12 - k * 0.07, lane, lane)
        cir.e(kart, pos=(x, 0, z), tags=["kart"], vars={"t": -0.12 - k * 0.07, "lane": lane, "base": 0.42 - k * 0.006})
        cir.e(f"{kart} Body", "cube", c, (0, 0.38, 0), scale=(1.2, 0.35, 2.0), parent=kart, metallic=0.5, roughness=0.35, clearcoat=1)
        cir.e(f"{kart} Nose", "cube", c, (0, 0.32, -1.15), scale=(0.9, 0.22, 0.5), parent=kart, metallic=0.5, roughness=0.35, clearcoat=1)
        cir.e(f"{kart} Spoiler", "cube", "#1a1a1a", (0, 0.75, 1.0), scale=(1.4, 0.08, 0.35), parent=kart)
        cir.e(f"{kart} Helmet", "sphere", c, (0, 0.85, 0.2), scale=0.5, parent=kart, metallic=0.4, roughness=0.15)
        cir.e(f"{kart} Visor", "sphere", "#111827", (0, 0.88, -0.02), scale=(0.42, 0.22, 0.2), parent=kart, metallic=0.8, roughness=0.1)
        cir.e(f"{kart} Number", "cube", "#ffffff", (0, 0.565, -0.2), scale=(0.5, 0.02, 0.5), parent=kart)
        for wx, wz in [(-0.7, -0.75), (0.7, -0.75), (-0.7, 0.75), (0.7, 0.75)]:
            cir.e(f"{kart} Wheel {wx}{wz}", "cylinder", pos=(wx, 0.28, wz), rot=(0, 0, 90), scale=(0.56, 0.3, 0.56), parent=kart,
                  material="materials/tire.mat.json", tags=["wheel"])
        for s in (-1, 1):
            cir.e(f"{kart} Lamp {s}", "sphere", "#000000", (s * 0.35, 0.38, -1.42), scale=0.12, parent=kart, emissive=[1, 0.95, 0.8, 5], unlit=True)
            cir.e(f"{kart} Tail {s}", "sphere", "#000000", (s * 0.45, 0.42, 1.02), scale=0.1, parent=kart, emissive=[1, 0.1, 0.05, 4], unlit=True)
    cir.e("Fireworks", pos=(0, 0, 0))
    cir.flush("Four karts on the grid")

    # --- Aurora: dusk + floodlights -------------------------------------------------------------
    aur.call("environment_update", skyMode="atmosphere", clouds=0.3, stars=0.4, skyTop="#1a1c4a", skyHorizon="#ff8a5a",
             ground="#2a2430", ambient=0.4, tonemap="agx", ao=1.0,
             sunAzimuth=260, sunElevation=2, sunColor="#ff9a6a", sunIntensity=2.4, fogColor="#6a4a6a",
             fogDensity=0.006, exposure=1.2, showGrid=False, bloomIntensity=0.65, bloomThreshold=0.95,
             saturation=1.12, contrast=1.08, vignette=0.35)
    for k, (x, z) in enumerate([(-A - 9, -B - 6), (A + 9, -B - 6), (-A - 9, B + 6), (A + 9, B + 6)]):
        aur.light(f"Floodlight {k+1}", (x * 0.95, 13.5, z * 0.95), "#fff1dc", 7, 60, kind="spot", spot=38,
                  rot=(-38, -math.degrees(math.atan2(-x, -z)) + 180, 0))
    aur.light("Gantry Glow", (A, 4.5, 0.6), "#ffd08a", 2.5, 8)
    aur.flush("Dusk sky and floodlights")

    # --- Stratus: racing AI -------------------------------------------------------------------
    for k, (name, c) in enumerate(KARTS):
        stra.behave(f"Kart {name}", "Race", "Race around the oval in its lane, speeding up and slowing so positions change; lean into the turns.", f"""
behavior Race
  on tick
    let speed = self.base + sin(time * 0.37 + {k * 1.9}) * 0.035
    self.t = self.t + speed * dt
    let lane = self.lane + sin(time * 0.5 + {k}) * 0.8
    self.position = (cos(self.t) * ({A} + lane), 0, sin(self.t) * ({B} + lane))
    let ahead = (cos(self.t + 0.04) * ({A} + lane), 0, sin(self.t + 0.04) * ({B} + lane))
    look self at ahead
    rotate self by (0, 0, 0)
  end
end""")
    stra.behave("Fireworks", "Show", "Launch fireworks over the stadium every so often.", """
on tick
  every 0.7 seconds
    let p = (random(-24, 24), random(16, 24), random(-30, -12))
    if chance(0.33) then
      spawn("prefab:prefabs/firework_gold.prefab.json", p)
    elif chance(0.5) then
      spawn("prefab:prefabs/firework_pink.prefab.json", p)
    else
      spawn("prefab:prefabs/firework_cyan.prefab.json", p)
    end
  end
end""")
    stra.flush("Racing AI and fireworks")
    for i in range(1, 111):
        stra.behave(f"Fan {i}", "Cheer", "Bounce and cheer.", """
on tick
  self.position.y = self.position.y + sin(time * 7 + self.phase) * 0.012
end""")
    stra.flush("Crowd")
    stra.e("Game Camera", pos=(A + 6, 3, -4), camera={"fov": 58, "primary": True})
    stra.flush("Chase camera")
    stra.behave("Game Camera", "Chase", "Ride just behind and above the leading kart.", f"""
on tick
  let k = find("Kart Comet")
  let t = k.t - 0.16
  self.position = lerp(self.position, (cos(t) * ({A} + 4.5), 3.2, sin(t) * ({B} + 4.5)), 0.12)
  look self at k.position + (0, 0.8, 0)
end""")
    stra.flush("Camera")
    nim.call("viewport_multi", size=256)
    nim.call("scene_save", path="scenes/main.sky.json")


def shots():
    def stadium(i, n):
        t = i / (n - 1)
        a = math.radians(110 + 40 * t)
        return dict(eye=[math.cos(a) * 46, 18 - 4 * t, math.sin(a) * 34], target=[0, 2, -4])

    def trackside(i, n):
        t = i / (n - 1)
        return dict(eye=[A + 9, 1.4, 12 - 20 * t], target=[A - 1, 0.8, 2 - 8 * t])

    return [dict(name="stadium", frames=150, cam=stadium, warmup=120),
            dict(name="trackside", frames=120, cam=trackside, warmup=60),
            dict(name="play", frames=210, view="scene", warmup=90)]
