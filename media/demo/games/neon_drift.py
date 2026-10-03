"""Neon Drift — a synthwave twin-stick arena shooter. Bullets, drones and explosions are prefabs
spawned at runtime from Wander."""
import math

META = dict(
    id="neon_drift", title="Neon Drift", genre="Arena Shooter", mood="neon",
    pitch="One ship. Endless drones. Don't stop moving.",
    view=dict(eye=[0, 22, 20], target=[0, 0, -2]),
)

HALF = 15


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Neon Drift", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])

    pix.material("materials/neon_pink.mat.json", color="#000000", emissive=[1, 0.16, 0.7, 1.4], unlit=True)
    pix.material("materials/neon_amber.mat.json", color="#000000", emissive=[1, 0.62, 0.08, 3.5], unlit=True)
    pix.material("materials/neon_cyan.mat.json", color="#000000", emissive=[0.1, 0.9, 1, 3.2], unlit=True)
    pix.material("materials/hull.mat.json", color="#1a1530", metallic=0.85, roughness=0.25, clearcoat=0.6)
    floor = pix.texture("tiles", "arena_tiles", color1="#0c0a16", color2="#100c1c", color3="#05040a", roughness=0.18, tiling=0.5)

    # --- Pixel: runtime prefabs (bolt, drone, explosion) ----------------------------------------
    pix.e("Bolt", tags=["bolt"], vars={"life": 1.1})
    pix.e("Bolt Core", "capsule", pos=(0, 0, 0), rot=(90, 0, 0), scale=(0.22, 1.3, 0.22), parent="Bolt", material="materials/neon_cyan.mat.json")
    pix.flush("Bolt model")
    pix.behave("Bolt", "Fly", "Fly straight ahead fast; hit the first drone in reach; fizzle out after a second.", """
on tick
  move self by forward(self) * 30 * dt
  self.life = self.life - dt
  let e = nearest("enemy")
  if exists(e) and distance(self, e) < 1.1 then
    emit "hit" to e
    destroy self
  elif self.life < 0 then
    destroy self
  end
end""")
    pix.flush("Bolt behavior")
    pix.prefab("Bolt", "prefabs/bolt.prefab.json", "Player plasma bolt (flies forward, hits enemies)", ["shooter", "projectile"])

    pix.e("Boom", tags=["fx"], vars={"t": 0})
    pix.e("Boom Core", "sphere", [1, 0.6, 0.2, 0.9], (0, 0, 0), scale=0.5, parent="Boom", emissive=[1, 0.5, 0.15, 6], unlit=True)
    pix.e("Boom Ring", "torus", [1, 0.2, 0.8, 0.9], (0, 0, 0), scale=(1.0, 0.3, 1.0), parent="Boom", emissive=[1, 0.2, 0.8, 5], unlit=True)
    pix.flush("Explosion model")
    pix.behave("Boom", "Burst", "Flash, expand and fade in under half a second.", """
on tick
  self.t = self.t + dt
  let k = self.t / 0.45
  self.scale = (1 + k * 4, 1 + k * 4, 1 + k * 4)
  if k > 1 then
    destroy self
  end
end""")
    pix.flush("Explosion behavior")
    pix.prefab("Boom", "prefabs/boom.prefab.json", "Neon explosion burst", ["shooter", "fx"])

    pix.e("Drone", tags=["enemy"], vars={"hp": 2, "speed": 3.4})
    pix.e("Drone Body", "sphere", pos=(0, 0, 0), scale=(0.9, 0.7, 0.9), parent="Drone", material="materials/hull.mat.json")
    pix.e("Drone Eye", "torus", pos=(0, 0, 0), scale=(1.1, 0.5, 1.1), parent="Drone", material="materials/neon_amber.mat.json")
    for k in range(4):
        a = k * math.pi / 2
        pix.e(f"Drone Spike {k+1}", "cone", "#2a1a40", (math.cos(a) * 0.55, 0, math.sin(a) * 0.55), rot=(0, -math.degrees(a), -90),
              scale=(0.25, 0.5, 0.25), parent="Drone", metallic=0.9, roughness=0.3)
    pix.flush("Drone model")
    pix.behave("Drone", "Hunt", "Home in on the player, spinning; two hits and it bursts.", """
behavior Hunt
  on tick
    let p = find("Player")
    if exists(p) then
      move self toward p at self.speed
    end
    rotate self by (0, 200 * dt, 0)
  end
  on event "hit"
    self.hp = self.hp - 1
    self.scale = (1.3, 1.3, 1.3)
    if self.hp <= 0 then
      spawn("prefab:prefabs/boom.prefab.json", self.position)
      emit "kill" to find("Player")
      destroy self
    end
  end
end""")
    pix.flush("Drone behavior")
    pix.prefab("Drone", "prefabs/drone.prefab.json", "Homing enemy drone (2 HP, explodes)", ["shooter", "enemy"])

    # --- Cirro: arena ------------------------------------------------------------------------------
    cir.e("Floor", "plane", pos=(0, 0, 0), scale=(400, 1, 400), material=floor, tags=["terrain"])
    for k in range(-HALF, HALF + 1, 3):
        mat = "materials/neon_cyan.mat.json" if abs(k) == HALF else "materials/neon_pink.mat.json"
        cir.e(f"Grid X {k}", "cube", pos=(k, 0.01, 0), scale=(0.05, 0.02, HALF * 2), material=mat, tags=["grid"])
        cir.e(f"Grid Z {k}", "cube", pos=(0, 0.01, k), scale=(HALF * 2, 0.02, 0.05), material=mat, tags=["grid"])
    for k, (x, z) in enumerate([(-HALF, -HALF), (HALF, -HALF), (-HALF, HALF), (HALF, HALF)]):
        cir.e(f"Pylon {k+1}", "cylinder", pos=(x, 2.5, z), scale=(1.2, 5, 1.2), material="materials/hull.mat.json")
        for j in range(3):
            cir.e(f"Pylon {k+1} Ring {j+1}", "cylinder", pos=(x, 1 + j * 1.6, z), scale=(1.3, 0.12, 1.3),
                  material="materials/neon_cyan.mat.json" if j % 2 else "materials/neon_pink.mat.json")
    cir.flush("Arena grid and pylons")
    cir.e("Retro Sun", "cylinder", "#000000", (0, 14, -90), rot=(90, 0, 0), scale=(46, 0.1, 46), emissive=[1, 0.36, 0.42, 1.0], unlit=True)
    for k in range(6):
        cir.e(f"Sun Stripe {k+1}", "cube", "#120326", (0, 4 + k * 2.6, -89), scale=(50, 0.5 + k * 0.18, 0.5))
    for k in range(9):
        x = -60 + k * 15
        cir.e(f"Mountain {k+1}", "cone", "#0d0420", (x, 6, -70 + (k % 2) * 6), scale=(26, 12 + (k % 3) * 5, 14), roughness=0.6)
        cir.e(f"Mountain Edge {k+1}", "cone", "#000000", (x, 6.05, -70 + (k % 2) * 6 + 0.1), scale=(26.3, 12.15 + (k % 3) * 5, 13.9),
              emissive=[0.45, 0.1, 0.9, 0.35], unlit=True)
    cir.flush("Retro sun and neon mountains")

    cir.e("Player", pos=(0, 0.8, 4), tags=["player"], vars={"score": 0})
    cir.e("Ship Hull", "cone", "#dfe8ff", (0, 0, 0), rot=(-90, 0, 0), scale=(0.8, 1.9, 0.5), parent="Player", metallic=0.5, roughness=0.3)
    cir.e("Ship Wings", "cube", pos=(0, -0.05, 0.35), scale=(2.2, 0.08, 0.7), parent="Player", material="materials/hull.mat.json")
    cir.e("Ship Trim", "cube", pos=(0, 0.0, 0.35), scale=(2.25, 0.03, 0.12), parent="Player", material="materials/neon_cyan.mat.json")
    cir.e("Ship Engine", "sphere", pos=(0, 0, 0.95), scale=(0.35, 0.25, 0.25), parent="Player", material="materials/neon_cyan.mat.json")
    cir.e("Spawner", pos=(0, 0.8, 0))
    cir.flush("Player ship")

    # --- Aurora: synthwave night ------------------------------------------------------------------------
    aur.call("environment_update", skyTop="#07011a", skyHorizon="#c0306a", ground="#12041e", ambient=0.35,
             sunAzimuth=180, sunElevation=6, sunColor="#ff6fa0", sunIntensity=0.6, fogColor="#3a0c4a",
             fogDensity=0.011, exposure=1.15, showGrid=False, bloomIntensity=0.85, bloomThreshold=0.9,
             saturation=1.08, contrast=1.12, vignette=0.4, stars=0.5, ao=0.8)
    aur.light("Ship Light", (0, 0.6, 0), "#43e8ff", 3.5, 8, parent="Player")
    for k, (x, z) in enumerate([(-HALF, -HALF), (HALF, -HALF), (-HALF, HALF), (HALF, HALF)]):
        aur.light(f"Pylon Light {k+1}", (x, 3, z), "#ff3dc8" if k % 2 else "#3de0ff", 4, 14)
    aur.flush("Neon lighting")

    # --- Stratus: gameplay ------------------------------------------------------------------------
    stra.behave("Player", "Pilot", "Strafe around the arena in loops, always aiming at the nearest drone, firing a steady stream.", f"""
behavior Pilot
  on tick
    let t = time * 0.55
    self.position = (sin(t) * 9, 0.8, sin(t * 2) * 6 + 2)
    let e = nearest("enemy")
    if exists(e) then
      look self at (e.position.x, 0.8, e.position.z)
    end
    every 0.11 seconds
      let b = spawn("prefab:prefabs/bolt.prefab.json", self.position + forward(self) * 1.3)
      b.rotation = self.rotation
    end
  end
  on event "kill"
    self.score = self.score + 1
  end
end""")
    stra.behave("Spawner", "Waves", "Send drones in from a random point on the arena edge, keeping the pressure on.", f"""
on tick
  every 0.45 seconds
    if count("enemy") < 16 then
      let a = random(0, 6.283)
      spawn("prefab:prefabs/drone.prefab.json", (cos(a) * {HALF + 1}, 0.8, sin(a) * {HALF + 1}))
    end
  end
end""")
    stra.behave("Ship Engine", "Thrust", "Engine glow pulses.", """
on tick
  let p = 0.3 + sin(time * 30) * 0.05
  self.scale = (p, 0.25, p)
end""")
    stra.e("Game Camera", pos=(0, 21, 17), camera={"fov": 48, "primary": True})
    stra.flush("Pilot AI, waves, thrust")
    stra.behave("Game Camera", "Track", "Hover high above the arena, leaning toward the player.", """
on tick
  let p = find("Player")
  self.position = lerp(self.position, (p.position.x * 0.4, 20, p.position.z * 0.4 + 16), 0.05)
  look self at (p.position.x * 0.5, 0, p.position.z * 0.5 - 2)
end""")
    stra.flush("Camera")
    nim.call("viewport_multi", size=256)
    nim.call("scene_save", path="scenes/main.sky.json")


def shots():
    def low(i, n):
        t = i / (n - 1)
        a = math.radians(-60 + 50 * t)
        return dict(eye=[math.sin(a) * 18, 2.2, math.cos(a) * 18], target=[0, 1.5, -4])

    def sky(i, n):
        t = i / (n - 1)
        return dict(eye=[-6 + 12 * t, 4, 22], target=[0, 8, -60])

    return [dict(name="low", frames=150, cam=low, warmup=240),
            dict(name="horizon", frames=120, cam=sky, warmup=120),
            dict(name="play", frames=210, view="scene", warmup=180)]
