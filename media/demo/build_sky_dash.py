#!/usr/bin/env python3
"""Builds "Sky Dash", a 2D side-scroller sample, entirely through engine tools + Wander.

The player auto-runs and jumps on a fixed rhythm; coins sit on the jump arcs, slimes and
spikes sit under them. Everything is deterministic, so the run replays identically.
Usage: build_sky_dash.py PROJECT_DIR
"""
import sys
from sky import Sky

project = sys.argv[1]
sky = Sky(project=project)
sky.call("scene_new", name="Sky Dash", empty=True)

RUN, JUMP_V, G, PERIOD, X0 = 5.0, 9.0, 22.0, 1.4, -6.0
airtime = 2 * JUMP_V / G
apex_h = JUMP_V * JUMP_V / (2 * G)

ops = []
def add(**a): ops.append(("entity_create", a))

# --- background layers (z < 0 so they sit behind gameplay at z = 0) ---
for i, (x, h, c) in enumerate([(-12, 6, "#7d8fc7"), (-3, 8, "#6f82bd"), (7, 5.5, "#7d8fc7"), (17, 7.5, "#6879b8"),
                                (28, 6, "#7d8fc7"), (39, 8.5, "#6f82bd"), (51, 6, "#7d8fc7"), (63, 7.5, "#6879b8")]):
    add(name=f"Mountain {i+1}", mesh="cone", color=c, position=[x, h / 2 - 0.6, -14], scale=[h * 1.6, h, 2],
        tags=["parallax"], vars={"factor": 0.6}, components={"mesh": {"roughness": 1}})
for i, x in enumerate(range(-10, 80, 7)):
    add(name=f"Hill {i+1}", mesh="sphere", color="#4f9e6a" if i % 2 else "#5aab72", position=[x, -1.2, -6],
        scale=[9, 5, 1], tags=["parallax"], vars={"factor": 0.3}, components={"mesh": {"roughness": 1}})
for i, (x, y) in enumerate([(-4, 8), (8, 9.5), (19, 7.8), (31, 9), (44, 8.4), (57, 9.6), (70, 8.2)]):
    add(name=f"Cloud {i+1}", position=[x, y, -8], tags=["cloud"], vars={"speed": 0.3 + 0.1 * (i % 3)})
sky.batch("Backdrop", ops); ops.clear()

for i in range(7):
    for j, (dx, dy, s) in enumerate([(0, 0, 1.6), (1.1, 0.3, 1.2), (-1.0, 0.1, 1.1), (0.4, 0.6, 1.0)]):
        add(name=f"Cloud {i+1} Puff {j+1}", parent=f"Cloud {i+1}", mesh="sphere", color="#ffffff",
            position=[dx, dy, 0], scale=[s * 1.4, s, 0.5], components={"mesh": {"roughness": 1}})
sky.batch("Cloud puffs", ops); ops.clear()

# --- ground & level ---
add(name="Ground", mesh="cube", color="#9b6b43", position=[35, -1.3, 0], scale=[100, 2.6, 3], components={"mesh": {"roughness": 1}})
add(name="Grass", mesh="cube", color="#4cb35a", position=[35, 0.0, 0], scale=[100, 0.4, 3.2], components={"mesh": {"roughness": 0.9}})
for i, x in enumerate(range(-8, 78, 5)):
    add(name=f"Bush {i+1}", mesh="sphere", color="#3f9a4e" if i % 2 else "#48a857", position=[x + (i * 7) % 3, 0.35, -0.9],
        scale=[1.6 + (i % 3) * 0.4, 1.0 + (i % 2) * 0.3, 0.6], components={"mesh": {"roughness": 1}})
for i, x in enumerate(range(-4, 78, 9)):
    add(name=f"Tree {i+1} Trunk", mesh="cylinder", color="#7a5235", position=[x, 1.0, -3], scale=[0.4, 2.2, 0.4])
    add(name=f"Tree {i+1} Crown", mesh="sphere", color="#2f8a4a" if i % 2 else "#3a9a52", position=[x, 2.8, -3], scale=[2.4, 2.4, 1])
jumps = []
for k in range(1, 9):
    t = PERIOD * k
    x_take = X0 + RUN * t
    jumps.append(x_take)
    x_apex = x_take + RUN * airtime / 2
    hazard = k % 2
    if hazard:
        add(name=f"Slime {k}", mesh="sphere", color="#e05d6f", position=[x_apex, 0.55, 0], scale=[1.0, 0.8, 0.8],
            tags=["enemy"], components={"mesh": {"roughness": 0.35}})
    else:
        add(name=f"Spikes {k}", mesh="cone", color="#7c7f8c", position=[x_apex, 0.6, 0], scale=[0.9, 0.9, 0.9],
            tags=["hazard"], components={"mesh": {"metallic": 0.6, "roughness": 0.35}})
    for c, frac in enumerate([0.3, 0.5, 0.7]):
        tt = airtime * frac
        add(name=f"Coin {k}.{c+1}", mesh="torus", color="#f4c430", position=[x_take + RUN * tt, 0.6 + JUMP_V * tt - 0.5 * G * tt * tt + 0.6, 0],
            rotation=[90, 0, 0], scale=[0.7, 0.7, 0.7], tags=["coin"], components={"mesh": {"metallic": 1, "roughness": 0.25, "emissive": "#ffb30033"}})
for i, x in enumerate([2.5, 16, 30, 44]):
    add(name=f"Platform {i+1}", mesh="cube", color="#8d6e52", position=[x, 3.6, -1.5], scale=[3, 0.5, 1], components={"mesh": {"roughness": 1}})
    add(name=f"Platform {i+1} Top", mesh="cube", color="#5fbf63", position=[x, 3.9, -1.5], scale=[3.1, 0.15, 1.05])
add(name="Goal Pole", mesh="cylinder", color="#e8e8ee", position=[X0 + RUN * PERIOD * 9 + 2, 2.5, 0], scale=[0.15, 5, 0.15])
add(name="Goal Flag", mesh="quad", color="#ff6b6b", position=[X0 + RUN * PERIOD * 9 + 2.75, 4.4, 0], scale=[1.4, 0.9, 1],
    components={"mesh": {"roughness": 0.8}})
sky.batch("Level layout", ops); ops.clear()

add(name="Player", mesh="capsule", color="#2ec4b6", position=[X0, 0.7, 0.5], scale=[1.1, 1.1, 1.1], tags=["player"],
    components={"mesh": {"roughness": 0.35}})
add(name="Player Eye", parent="Player", mesh="sphere", color="#ffffff", position=[0.12, 0.22, 0.2], scale=[0.18, 0.18, 0.1])
add(name="Game Camera", position=[X0 + 4, 3.4, 20], components={"camera": {"orthographic": True, "orthoSize": 5.4, "fov": 50}})
sky.batch("Player & camera", ops); ops.clear()

# --- behaviors (ECPS: intent + Wander) ---
def behave(entity, name, intent, source):
    r = sky.call("behavior_set", entity=entity, name=name, intent=intent, source=source)
    assert r["ok"], r

behave("Player", "Runner", "Run right at a steady pace and hop on a rhythm, landing back on the grass.", f"""
behavior Runner
  var vy = 0
  var coins = 0
  on tick
    move self by ({RUN} * dt, 0, 0)
    vy = vy - {G} * dt
    self.position.y = max(0.7, self.position.y + vy * dt)
    every {PERIOD} seconds
      vy = {JUMP_V}
    end
    -- squash & stretch while airborne
    let s = 1.1 + clamp(vy * 0.02, -0.12, 0.12)
    self.scale = (1.1 - (s - 1.1), s, 1.1)
  end
  on event "coin"
    coins = coins + 1
  end
end
""")

for k in range(1, 9):
    for c in range(1, 4):
        behave(f"Coin {k}.{c}", "Collectible", "Spin in place; when the player touches it, pop and count a coin.", """
on tick
  rotate self by (0, 240 * dt, 0)
  let p = find("Player")
  if exists(p) and distance(self, p) < 0.9 then
    emit "coin" to p
    destroy self
  end
end
""")
    if k % 2:
        behave(f"Slime {k}", "Squish", "Bounce in place with a jelly squish so it looks alive.", """
behavior Squish
  var base = 0
  on start
    base = self.position.y
  end
  on tick
    let t = sin(time * 6 + self.id)
    self.scale = (1.0 + t * 0.12, 0.8 - t * 0.12, 0.8)
    self.position.y = base - t * 0.06
  end
end
""")

behave("Game Camera", "Follow", "Follow the player smoothly, keeping them a little left of center.", """
on tick
  let p = find("Player")
  if exists(p) then
    self.position.x = lerp(self.position.x, p.position.x + 4, 0.08)
  end
end
""")
behave("Goal Flag", "Wave", "Flutter gently like a flag in the wind.", """
on tick
  rotate self by (0, sin(time * 4) * 30 * dt, 0)
end
""")
for i in range(7):
    behave(f"Cloud {i+1}", "Drift", "Drift slowly to the right, forever.", """
on tick
  move self by (self.speed * dt, 0, 0)
end
""")
for tag, factor in [("Mountain", 0.6), ("Hill", 0.3)]:
    count = 8 if tag == "Mountain" else 13
    for i in range(count):
        behave(f"{tag} {i+1}", "Parallax", "Slide with the camera at a fraction of its speed for depth.", """
behavior Parallax
  var home = (0, 0, 0)
  var cam0 = 0
  on start
    home = self.position
    cam0 = find("Game Camera").position.x
  end
  on tick
    let cam = find("Game Camera").position.x
    self.position.x = home.x + (cam - cam0) * self.factor
  end
end
""")

sky.call("environment_update", skyTop="#5aa2ff", skyHorizon="#8cc4ff", ground="#7d8f6a", ambient=0.75,
         sunAzimuth=20, sunElevation=40, sunIntensity=1.6, sunColor="#fff4e0", fogDensity=0, exposure=1.0, showGrid=False)
print(sky.text("scene_save", path="scenes/main.sky.json"))
sky.close()
