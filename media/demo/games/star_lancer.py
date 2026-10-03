"""Star Lancer — a side-scrolling space shooter over a ringed giant."""
import math
from kit import rnd

META = dict(
    id="star_lancer", title="Star Lancer", genre="Space Shoot 'em Up", mood="space",
    pitch="Hold the line above the ringed giant.",
    view=dict(eye=[0, 3, 34], target=[0, 0, -10]),
)


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Star Lancer", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])
    r = rnd(77)

    pix.material("materials/laser.mat.json", color="#000000", emissive=[1, 0.25, 0.3, 5], unlit=True)
    pix.material("materials/raider.mat.json", color="#3a1420", metallic=0.7, roughness=0.3)
    pix.e("Laser", tags=["laser"], vars={"life": 0.9})
    pix.e("Laser Beam", "cube", pos=(0, 0, 0), scale=(1.6, 0.09, 0.09), parent="Laser", material="materials/laser.mat.json")
    pix.flush("Laser model")
    pix.behave("Laser", "Fly", "Streak right; hit the first raider in reach.", """
on tick
  move self by (42 * dt, 0, 0)
  self.life = self.life - dt
  let e = nearest("enemy")
  if exists(e) and distance(self, e) < 1.3 then
    emit "hit" to e
    destroy self
  elif self.life < 0 then
    destroy self
  end
end""")
    pix.flush("Laser behavior")
    pix.prefab("Laser", "prefabs/laser.prefab.json", "Red laser bolt", ["space", "projectile"])
    pix.e("Blast", vars={"t": 0})
    pix.e("Blast Core", "sphere", [1, 0.8, 0.4, 0.85], (0, 0, 0), scale=0.6, parent="Blast", emissive=[1, 0.6, 0.2, 7], unlit=True)
    pix.e("Blast Shell", "sphere", [1, 0.3, 0.1, 0.35], (0, 0, 0), scale=1.0, parent="Blast", emissive=[1, 0.3, 0.1, 3], unlit=True)
    pix.flush("Blast model")
    pix.behave("Blast", "Burst", "Bloom outward and vanish.", """
on tick
  self.t = self.t + dt
  let k = self.t / 0.5
  self.scale = (1 + k * 3.5, 1 + k * 3.5, 1 + k * 3.5)
  move self by (-4 * dt, 0, 0)
  if k > 1 then
    destroy self
  end
end""")
    pix.flush("Blast behavior")
    pix.prefab("Blast", "prefabs/blast.prefab.json", "Space explosion", ["space", "fx"])
    pix.e("Raider", tags=["enemy"], vars={"phase": 0})
    pix.e("Raider Hull", "cone", pos=(0, 0, 0), rot=(0, 0, 90), scale=(1.0, 1.8, 0.7), parent="Raider", material="materials/raider.mat.json")
    pix.e("Raider Fins", "cube", pos=(0.4, 0, 0), scale=(0.5, 1.8, 0.08), parent="Raider", material="materials/raider.mat.json")
    pix.e("Raider Eye", "sphere", "#000000", (-0.35, 0.1, 0.3), scale=(0.3, 0.16, 0.12), parent="Raider", emissive=[0.3, 1, 0.4, 5], unlit=True)
    pix.e("Raider Engine", "sphere", "#000000", (0.85, 0, 0), scale=(0.2, 0.35, 0.35), parent="Raider", emissive=[0.4, 1, 0.5, 4], unlit=True)
    pix.flush("Raider model")
    pix.behave("Raider", "Attack", "Fly left in a sine wave; burst when shot.", """
behavior Attack
  on tick
    move self by (-7 * dt, cos(time * 2.2 + self.phase) * 3 * dt, 0)
    self.rotation = (sin(time * 2.2 + self.phase) * 25, 0, 0)
    if self.position.x < -30 then
      destroy self
    end
  end
  on event "hit"
    spawn("prefab:prefabs/blast.prefab.json", self.position)
    emit "kill" to find("Lancer")
    destroy self
  end
end""")
    pix.flush("Raider behavior")
    pix.prefab("Raider", "prefabs/raider.prefab.json", "Enemy raider (sine-wave attacker)", ["space", "enemy"])

    # --- Cirro: the system ---------------------------------------------------------------------
    for k in range(320):
        z = r.uniform(-140, -90)
        c = r.choice([[1, 1, 1], [0.7, 0.8, 1], [1, 0.85, 0.7]])
        cir.e(f"Star {k+1}", "sphere", "#000000", (r.uniform(-170, 170), r.uniform(-80, 90), z), scale=r.uniform(0.15, 0.45),
              emissive=c + [r.uniform(1.5, 4)], unlit=True, tags=["star"], vars={"speed": 0.5 + (z + 140) * 0.02})
    cir.flush("Starfield")
    bands = cir.call("texture_generate", kind="stripes", name="textures/gas_bands", color1="#e0a46a", color2="#b8663e",
                     color3="#f2d2a0", scale=9, variation=0.8, bump=0.2, create_material=False)
    cir.e("Planet", "sphere", pos=(48, -40, -120), rot=(0, 0, 90), scale=74, texture=bands["albedo"], roughness=0.8, tags=["planet"])
    cir.e("Atmosphere", "sphere", [1, 0.75, 0.55, 0.1], (48, -40, -120), scale=78, emissive=[1, 0.6, 0.35, 0.18], unlit=True)
    cir.e("Rings", pos=(48, -40, -120), rot=(12, 0, 14))
    for band, (rad, w, c) in enumerate([(52, 9, [0.92, 0.8, 0.62, 0.75]), (66, 12, [0.8, 0.62, 0.48, 0.55])]):
        for k in range(72):
            a_ = 2 * math.pi * k / 72
            cir.e(f"Ring {band+1}.{k+1}", "cube", c, (math.cos(a_) * rad, 0, math.sin(a_) * rad), rot=(0, -math.degrees(a_), 0),
                  scale=(w, 0.25, 2 * math.pi * rad / 72 + 0.3), parent="Rings", emissive=c[:3] + [0.12], roughness=0.9)
    cir.e("Moon", "sphere", "#9aa0a8", (-34, 22, -80), scale=8, roughness=0.95)
    cir.flush("Ringed giant and moon")
    for k in range(14):
        a = f"Asteroid {k+1}"
        s = r.uniform(0.6, 2.2)
        cir.e(a, "sphere", r.choice(["#5a524c", "#6a625a", "#4a4440"]), (r.uniform(-28, 28), r.uniform(-11, 11), r.uniform(-14, -4)),
              rot=(r.uniform(0, 360), r.uniform(0, 360), 0), scale=(s * 1.3, s, s * 1.1), roughness=1, tags=["asteroid"],
              vars={"spin": r.uniform(-60, 60), "drift": r.uniform(1.5, 4)})
    cir.e("Lancer", pos=(-9, 0, 0), tags=["player"], vars={"score": 0})
    cir.e("Lancer Hull", "capsule", "#f0f2f6", (0, 0, 0), rot=(0, 0, 90), scale=(0.9, 3.0, 0.7), parent="Lancer", metallic=0.3, roughness=0.35, clearcoat=1)
    cir.e("Lancer Stripe", "capsule", "#ff8a2a", (0.1, 0.12, 0), rot=(0, 0, 90), scale=(0.6, 2.4, 0.74), parent="Lancer", roughness=0.4)
    cir.e("Lancer Cockpit", "sphere", [0.4, 0.85, 1, 0.8], (0.55, 0.28, 0), scale=(0.7, 0.32, 0.36), parent="Lancer", metallic=0.3, roughness=0.05,
          emissive=[0.2, 0.6, 1, 0.6])
    for s in (-1, 1):
        cir.e(f"Lancer Wing {'L' if s < 0 else 'R'}", "cube", "#d8dce4", (-0.3, -0.1, s * 0.9), rot=(0, s * 25, 0),
              scale=(1.3, 0.08, 1.5), parent="Lancer", metallic=0.4, roughness=0.3)
    cir.e("Lancer Engine", "sphere", "#000000", (-1.55, 0, 0), scale=(0.5, 0.42, 0.42), parent="Lancer", emissive=[1, 0.55, 0.15, 6], unlit=True)
    cir.e("Spawner", pos=(30, 0, 0))
    cir.flush("Asteroids and the Lancer")

    # --- Aurora: starlight ------------------------------------------------------------------------
    aur.call("environment_update", stars=1.0, skyTop="#010108", skyHorizon="#070418", ground="#020108", ambient=0.18,
             sunAzimuth=70, sunElevation=12, sunColor="#ffe2c0", sunIntensity=2.4, fogDensity=0, exposure=1.2,
             showGrid=False, bloomIntensity=0.8, bloomThreshold=0.9, saturation=1.1, contrast=1.1, vignette=0.45)
    aur.light("Engine Light", (-1.8, 0, 0), "#ff9a40", 3, 6, parent="Lancer")
    aur.light("Cockpit Light", (1, 0.6, 0.8), "#6fc8ff", 1, 3, parent="Lancer")
    aur.flush("Starlight and engine glow")

    # --- Stratus -----------------------------------------------------------------------------------
    stra.behave("Lancer", "Pilot", "Weave up and down, firing a steady laser stream.", """
behavior Pilot
  on tick
    self.position = (-9 + sin(time * 0.4) * 2, sin(time * 0.75) * 4.5, 0)
    self.rotation = (cos(time * 0.75) * -18, 0, cos(time * 0.75) * 8)
    every 0.13 seconds
      spawn("prefab:prefabs/laser.prefab.json", self.position + (1.9, 0, 0))
    end
  end
  on event "kill"
    self.score = self.score + 1
  end
end""")
    stra.behave("Spawner", "Waves", "Raiders arrive in short waves from the right.", """
on tick
  every 0.55 seconds
    let y = random(-7, 7)
    let r = spawn("prefab:prefabs/raider.prefab.json", (30, y, 0))
    r.phase = random(0, 6.28)
  end
end""")
    for k in range(14):
        stra.behave(f"Asteroid {k+1}", "Tumble", "Tumble and drift left; wrap around.", """
on tick
  rotate self by (self.spin * dt, self.spin * 0.6 * dt, 0)
  move self by (0 - self.drift * dt, 0, 0)
  if self.position.x < -34 then
    self.position.x = 34
  end
end""")
    stra.flush("Pilot, raider waves, asteroids")
    for k in range(320):
        stra.behave(f"Star {k+1}", "Parallax", "Slide left, nearer stars faster.", """
on tick
  move self by (0 - self.speed * dt, 0, 0)
  if self.position.x < -170 then
    self.position.x = 170
  end
end""")
    stra.flush("Star parallax")
    stra.e("Game Camera", pos=(2, 2, 24), camera={"fov": 42, "primary": True})
    stra.flush("Game camera")
    stra.behave("Game Camera", "Drift", "Hold the battle in frame with a gentle drift.", """
on tick
  self.position = (1 + sin(time * 0.2) * 1.5, 1.5 + sin(time * 0.15) * 1, 23)
  look self at (2, 0, -6)
end""")
    stra.flush("Camera")
    nim.call("viewport_multi", size=256)
    nim.call("scene_save", path="scenes/main.sky.json")


def shots():
    def flyby(i, n):
        t = i / (n - 1)
        e = t * t * (3 - 2 * t)
        return dict(eye=[-20 + 10 * e, 4 - 2 * e, 9 - 2 * e], target=[10, -4, -40])

    def wide(i, n):
        t = i / (n - 1)
        return dict(eye=[-6 + 6 * t, 10 - 4 * t, 40], target=[10, -6, -40])

    return [dict(name="flyby", frames=150, cam=flyby, warmup=200),
            dict(name="wide", frames=135, cam=wide, warmup=240),
            dict(name="play", frames=210, view="scene", warmup=200)]
