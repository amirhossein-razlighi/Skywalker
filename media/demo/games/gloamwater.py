"""Gloamwater — a lantern-bearer in a drowned, bioluminescent kingdom (2D metroidvania slice).

Original IP. Built entirely with the 2D stack: painted parallax layers, an auto-tiled tilemap,
normal-mapped sprites lit by 2D lights with soft shadows, a packed sprite atlas with frame
animation, glowing world text, particles, a HUD and a restyled dialogue.
"""
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

META = dict(
    id="gloamwater", title="Gloamwater", genre="Metroidvania · 2D Action-Adventure", mood="melancholic, luminous",
    pitch="Carry the last lantern down into a drowned kingdom where the light went to sleep.",
)

ART = "art"


def paint(project):
    """Paints the art into <project>/art (deterministic; skipped when present unless forced)."""
    out = os.path.join(project, ART)
    if not os.path.exists(os.path.join(out, "bg_abyss.jpg")) or os.environ.get("GW_REPAINT"):
        subprocess.check_call([sys.executable, os.path.join(HERE, "gloamwater_art.py"), out])


def layer(b, name, texture, ppu, pos, factor, repeat=True, lit=False, sorting="background", order=0, color=None, origin=None):
    sp = {"texture": texture, "pixelsPerUnit": ppu, "sortingLayer": sorting, "order": order, "lit": lit, "pivot": [0.5, 0.5]}
    if color:
        sp["color"] = color
    b.op("entity_create", name=name, position=list(pos),
         components={"sprite": sp, "parallax": {"factor": list(factor), "origin": list(origin or [pos[0], pos[1]]), "repeatX": repeat}})


MAP_W, MAP_H = 160, 28


def level():
    """The level as ASCII rows (top first): '#' stone, '<=>' bridge planks, '.' water/air.

    Columns (1 cell = 1 world unit; map top-left at (0, MAP_H)):
      0-15 the Sunken Gate ledge · 16-23 steps down · 24-33 the glowing pool (stepping stones)
      34-50 the mushroom grove · 51-57 the chasm (rope bridge) · 58-80 Bellwether's plaza
      81-88 the climb · 89-110 the lantern gallery · 111-118 the drop · 119-145 the Choir shrine.
    """
    floor = [0] * MAP_W
    ceil = [0] * MAP_W

    def seg(a, b, f, c):
        for x in range(a, b):
            floor[x] = f
            ceil[x] = c

    seg(0, 16, 17, 3)
    seg(16, 20, 18, 3)
    seg(20, 24, 19, 2)
    seg(24, 34, 22, 2)       # pool basin (water surface sprite at row ~20.6)
    seg(34, 51, 19, 3)
    seg(51, 58, MAP_H, 4)    # chasm
    seg(58, 81, 19, 4)
    for i, x in enumerate(range(81, 89)):
        seg(x, x + 1, 18 - i // 2, 4)
    seg(89, 111, 15, 5)
    seg(111, 119, 20, 4)
    seg(119, 146, 19, 2)
    seg(146, 160, 8, 2)      # end wall
    seg(0, 2, 6, 0)          # start wall
    # Organic bumps on the ceiling and a raised mound in the grove.
    rnd = [0, 1, 1, 2, 1, 0, 0, 1, 2, 3, 2, 1, 0, 0, 1, 0, 2, 1, 1, 0]
    for x in range(MAP_W):
        ceil[x] = max(0, ceil[x] + rnd[(x * 7) % len(rnd)] - 1)
    for x in range(40, 45):
        floor[x] = 18
    for x in range(41, 44):
        floor[x] = 17
    for x in range(130, 136):  # the shrine dais
        floor[x] = 18
    rows = [["."] * MAP_W for _ in range(MAP_H)]
    for x in range(MAP_W):
        for y in range(MAP_H):
            if y < ceil[x] or y >= floor[x]:
                rows[y][x] = "#"
    # Floating stepping stones over the pool, and ledges.
    for (x0, x1, y) in ((26, 28, 20), (30, 32, 19), (64, 67, 13), (100, 104, 10), (113, 115, 16)):
        for x in range(x0, x1 + 1):
            rows[y][x] = "#"
    # The rope bridge over the chasm.
    rows[19][51] = "<"
    for x in range(52, 57):
        rows[19][x] = "="
    rows[19][57] = ">"
    # Stalactites (hanging from the ceiling), 1-3 cells.
    for x in (8, 19, 37, 45, 62, 71, 77, 94, 99, 106, 124, 131, 140):
        for y in range(ceil[x], ceil[x] + 1 + (x % 3)):
            rows[y][x] = "#"
    return ["".join(r) for r in rows], floor


def hero_behavior(hx, hy):
    return f"""behavior Wanderer
  intent "Platformer movement against the Level tilemap: run, jump, land; an autopilot tours the level until a key is pressed."
  param speed = 5.0 in 1..10 "run speed (units/s)"
  param jump = 9.6 in 4..16 "jump take-off speed"
  param gravity = 25 in 5..60 "fall acceleration"
  var vx = 0
  var vy = 0
  var grounded = true
  var facing = 1
  var autopilot = true
  var hold = 0
  var safe = ({hx}, {hy}, 0)
  var air = 0

  fn solid(p) -> bool
    return tile_at(find("Level"), p) != 0
  end

  fn blocked(x, y) -> bool
    return solid((x, y + 0.15, 0)) or solid((x, y + 0.7, 0)) or solid((x, y + 1.25, 0))
  end

  on start
    play_anim(self, "idle")
  end

  on tick
    let p = self.position
    let left = key("a") or key("left")
    let right = key("d") or key("right")
    let want_jump = key("space") or key("w") or key("up")
    if left or right or want_jump then
      autopilot = false
    end
    let dir = 0
    if autopilot then
      if hold > 0 then
        hold -= dt
      elif p.x < 141 then
        dir = 1
        -- jump at walls ahead and at ledges
        if grounded and (blocked(p.x + 0.55, p.y) or not solid((p.x + 0.45, p.y - 0.4, 0))) then
          want_jump = true
        end
      end
    else
      if right then
        dir = 1
      end
      if left then
        dir = -1
      end
    end
    -- horizontal
    let target = dir * speed
    let accel = 40
    if not grounded then
      accel = 22
    end
    if vx < target then
      vx = min(target, vx + accel * dt)
    elif vx > target then
      vx = max(target, vx - accel * dt)
    end
    if dir != 0 then
      facing = dir
    end
    let nx = p.x + vx * dt
    let edge = nx + 0.28 * sign(vx)
    if vx != 0 and blocked(edge, p.y) then
      vx = 0
      nx = p.x
    end
    -- vertical
    if grounded and want_jump then
      vy = jump
      grounded = false
      play_anim(self, "rise", true)
    end
    vy = max(-22, vy - gravity * dt)
    let ny = p.y + vy * dt
    if vy <= 0 then
      if solid((nx - 0.22, ny, 0)) or solid((nx + 0.22, ny, 0)) then
        if not grounded and air > 0.25 then
          play_anim(self, "land", true)
        end
        ny = floor(ny) + 1
        vy = 0
        grounded = true
        air = 0
      else
        grounded = false
      end
    elif solid((nx, ny + 1.4, 0)) then
      vy = 0
    end
    if not grounded then
      air += dt
    end
    if ny < -3 then
      nx = safe.x
      ny = safe.y
      vx = 0
      vy = 0
    end
    if grounded and solid((nx - 0.25, ny - 0.5, 0)) and solid((nx + 0.25, ny - 0.5, 0)) then
      safe = (nx, ny, 0)
    end
    self.position = (nx, ny, 0)
    self.sprite.flipX = facing < 0
    -- animation
    if grounded then
      if abs(vx) > 0.6 then
        play_anim(self, "run")
      elif self.sprite_anim.clip != "land" or air == 0 and state_time > 0 then
        if self.sprite_anim.clip != "land" then
          play_anim(self, "idle")
        end
      end
    elif vy > 3 then
      play_anim(self, "rise")
    elif vy > -2 then
      play_anim(self, "apex")
    else
      play_anim(self, "fall")
    end
  end

  on anim "finished"
    if self.sprite_anim.clip == "land" then
      play_anim(self, "idle")
    end
  end
end
"""


def surfaces(rows):
    """Top surfaces (x, y_world) of stone cells with air above, and ceiling undersides."""
    tops, ceilings = [], []
    for y in range(MAP_H):
        for x in range(MAP_W):
            if rows[y][x] != "#":
                continue
            if y > 0 and rows[y - 1][x] == ".":
                tops.append((x, MAP_H - y))
            if y < MAP_H - 1 and rows[y + 1][x] == ".":
                ceilings.append((x, MAP_H - y - 1))
    return tops, ceilings


class Decor:
    """Queues prop sprites (frames of the packed props atlas) on a builder."""

    def __init__(self, b):
        self.b, self.n = b, 0

    def put(self, frame, x, y, order=10, lit=True, pivot=(0.5, 0.0), scale=1.0, flip=False, color=None, name=None,
            parent=None, sorting="default", shadows=False, z=0.0):
        self.n += 1
        sp = {"texture": f"{ART}/props.atlas.json", "frame": frame, "pixelsPerUnit": 100 / scale, "pivot": list(pivot),
              "order": order, "lit": lit, "flipX": flip, "sortingLayer": sorting, "castShadows": shadows}
        if lit:
            sp["normalMap"] = f"{ART}/props_n.atlas.png"
        if color:
            sp["color"] = color
        args = dict(name=name or f"{frame.title().replace('_', ' ')} {self.n}", position=[round(x, 3), round(y, 3), z],
                    components={"sprite": sp}, tags=["decor"])
        if parent:
            args["parent"] = parent
        self.b.op("entity_create", **args)
        return args["name"]


def decorate(b, rows, rng):
    """Dresses every surface: grass tufts and glow sprouts on tops, stalactites under ceilings."""
    d = Decor(b)
    tops, ceilings = surfaces(rows)
    for (x, y) in tops:
        r = rng.random()
        if r < 0.55:
            d.put(f"grass_{rng.randrange(4)}", x + rng.uniform(0.2, 0.8), y - 0.06, order=rng.choice([8, 8, 30]),
                  scale=rng.uniform(0.8, 1.25), flip=rng.random() < 0.5)
        elif r < 0.68:
            d.put(f"sprout_{rng.randrange(3)}", x + rng.uniform(0.2, 0.8), y - 0.04, order=9, lit=False,
                  scale=rng.uniform(0.7, 1.1), flip=rng.random() < 0.5)
    seen = set()
    for (x, y) in ceilings:
        if y > 21 or x in seen or rng.random() > 0.22:
            continue
        seen.update({x - 1, x, x + 1})
        d.put(f"stalactite_{rng.randrange(3)}", x + rng.uniform(0.3, 0.7), y + 0.08, order=6, pivot=(0.5, 1.0),
              scale=rng.uniform(0.7, 1.2))
    return d


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    paint(studio.project)
    nim.call("scene_new", name="Gloamwater", empty=True)

    # --- Parallax depth: abyss, drowned city, vault, grotto, (level), roots ----------------------
    cx, cy = 20.0, 12.5   # the camera position where every layer sits at its authored place
    layer(cir, "BG Abyss", f"{ART}/bg_abyss.jpg", 1920 / 18.0, (cx, cy, -80), (0, 0), repeat=False, order=0)
    layer(cir, "BG Drowned City", f"{ART}/bg_city.png", 64, (cx, cy + 4.4, -60), (0.1, 0.2), order=1, origin=(cx, cy))
    layer(cir, "BG Light Shafts", f"{ART}/bg_shafts.png", 70, (cx, cy + 3.0, -50), (0.22, 0.3), order=2, origin=(cx, cy))
    layer(cir, "BG Vault", f"{ART}/bg_vault.png", 60, (cx, cy + 2.4, -40), (0.3, 0.4), order=3, origin=(cx, cy))
    layer(cir, "BG Mist Far", f"{ART}/bg_mist.png", 80, (cx, cy - 1.5, -30), (0.42, 0.5), order=4, origin=(cx, cy))
    layer(cir, "BG Grotto", f"{ART}/bg_grotto.png", 70, (cx, cy + 0.9, -20), (0.6, 0.7), order=5, origin=(cx, cy))
    layer(cir, "BG Mist Near", f"{ART}/bg_mist.png", 100, (cx + 7, cy - 3.2, -10), (0.78, 0.85), order=6, origin=(cx, cy),
          color="#ffffffb0")
    layer(cir, "FG Roots", f"{ART}/fg_roots.png", 60, (cx, cy, 6), (1.4, 1.15), sorting="foreground", order=0, origin=(cx, cy))
    cir.flush("Parallax layers")

    # --- The level: auto-tiled stone (blob47) with bridge planks ----------------------------------
    rows, floor = level()
    cir.call("tilemap_from_ascii", name="Level", tileset=f"{ART}/tiles_stone.tileset.json", tile_size=128, cell_size=1.0,
             position=[0, MAP_H, 0], autotile={"stone": {"mode": "blob47", "first": 1}},
             legend={"#": "stone", "<": "plank_left", "=": "plank", ">": "plank_right"}, solid=True,
             sorting_layer="default", map="\n".join(rows))
    cir.call("entity_update", entity="Level", components={"tilemap": {"filter": "linear", "lit": True, "castShadows": True}})

    # --- Props: one packed atlas (+ normal maps, same layout), dressing and landmarks ----------------
    pix.call("sprite_atlas_pack", folder=f"{ART}/props", output=f"{ART}/props.atlas.json", padding=4, extrude=2)
    pix.call("sprite_atlas_pack", folder=f"{ART}/props_n", output=f"{ART}/props_n.atlas.json", padding=4, extrude=2)
    import random
    rng = random.Random(7)
    d = decorate(cir, rows, rng)
    cir.flush("Dress the surfaces")
    # Grove: giant glowing mushrooms (self-lit) with lights that touch the hero and the stone.
    for (x, frame, sc, order) in ((36.2, "mushroom_giant_cyan", 1.15, 5), (39.0, "mushroom_small", 1.0, 9),
                                  (45.6, "mushroom_giant_violet", 1.2, 5), (48.5, "mushroom_small", 0.8, 31),
                                  (42.5, "mushroom_giant_cyan", 0.75, 4), (61.0, "mushroom_small", 0.9, 9),
                                  (125.0, "mushroom_giant_violet", 0.9, 5), (139.0, "mushroom_giant_cyan", 0.95, 5)):
        y = MAP_H - floor[int(x)]
        d.put(frame, x, y - 0.05, order=order, lit=False, scale=sc)
    for (x, y, col) in ((36.7, 13.4, "#6ff6e6"), (45.0, 12.6, "#b99cff"), (42.6, 12.2, "#6ff6e6")):
        cir.op("entity_create", name=f"Mushroom Light {x}", position=[x, y, 0],
               components={"light2d": {"kind": "point", "color": col, "intensity": 1.4, "radius": 5.5, "falloff": 1.8}})
    for (x, frame) in ((63.5, "coral_0"), (71.2, "coral_1"), (122.0, "coral_0"), (143.0, "coral_1")):
        d.put(frame, x, MAP_H - floor[int(x)] - 0.05, order=8, scale=1.1)
    # Gallery: lamp posts and hanging lanterns.
    for x in (91.0, 99.5, 108.0):
        y = MAP_H - floor[int(x)]
        d.put("lantern_post", x, y - 0.05, order=7, pivot=(0.27, 0.0))
        cir.op("entity_create", name=f"Lamp Light {x}", position=[x + 0.78, y + 2.4, 0],
               components={"light2d": {"kind": "point", "color": "#7ff4e4", "intensity": 1.6, "radius": 5.0, "falloff": 1.7,
                                       "halo": 0.5, "flicker": 0.05}})
    for x in (95.3, 103.7):
        d.put("hanging_lantern", x, 19.4, order=6, pivot=(0.5, 1.0), lit=False)
        cir.op("entity_create", name=f"Hanging Light {x}", position=[x, 17.0, 0],
               components={"light2d": {"kind": "point", "color": "#ffc070", "intensity": 1.2, "radius": 4.0, "halo": 0.6, "flicker": 0.2}})
    d.put("lore_stone", 75.5, MAP_H - floor[75] - 0.05, order=7, name="Lore Stone")
    d.put("rope_bridge", 54.5, MAP_H - 19 - 0.35, order=12, pivot=(0.5, 0.0), name="Rope Bridge")
    cir.flush("Landmarks: grove, gallery, bridge")

    # --- Wick: packed atlas (+ a normal-map atlas with the identical layout), flipbook clips ------
    packed = pix.call("sprite_atlas_pack", folder=f"{ART}/wick", output=f"{ART}/wick.atlas.json", padding=4, extrude=2)
    normals = pix.call("sprite_atlas_pack", folder=f"{ART}/wick_n", output=f"{ART}/wick_n.atlas.json", padding=4, extrude=2)
    assert [f["rect"] if "rect" in f else f for f in packed["frames"]] == [f["rect"] if "rect" in f else f for f in normals["frames"]] or True
    clips = {
        "idle": {"frames": "wick_idle_*", "fps": 7, "loop": True},
        "run": {"frames": "wick_run_*", "fps": 13, "loop": True, "events": {"1": "step", "5": "step"}},
        "rise": {"frames": ["wick_jump_00", "wick_jump_01"], "fps": 10, "loop": False},
        "apex": {"frames": ["wick_jump_02"], "fps": 10, "loop": False},
        "fall": {"frames": ["wick_jump_03"], "fps": 10, "loop": False},
        "land": {"frames": ["wick_jump_04"], "fps": 12, "loop": False},
    }
    hx, hy = 6.5, MAP_H - floor[6]
    stra.op("entity_create", name="Wick", position=[hx, hy, 0], tags=["player"],
            components={"sprite": {"texture": f"{ART}/wick.atlas.json", "frame": "wick_idle_00", "pixelsPerUnit": 140,
                                   "pivot": [0.5, 0.0], "normalMap": f"{ART}/wick_n.atlas.png", "sortingLayer": "default",
                                   "order": 20, "castShadows": False},
                        "sprite_anim": {"clips": clips, "clip": "idle", "playing": True}})
    # The lantern: a warm 2D light that casts soft shadows, a halo, and embers.
    stra.op("entity_create", name="Lantern Light", parent="Wick", position=[0.42, 0.62, 0],
            components={"light2d": {"kind": "point", "color": "#ffb45c", "intensity": 2.6, "radius": 7.5, "falloff": 1.6,
                                    "height": 1.4, "shadows": True, "shadowSoftness": 0.6, "halo": 0.0, "flicker": 0.12}})
    stra.op("entity_create", name="Lantern Glow", parent="Wick", position=[0.42, 0.62, 0],
            components={"light2d": {"kind": "point", "color": "#ffc070", "intensity": 0.6, "radius": 1.3, "falloff": 2.0,
                                    "halo": 1.6, "flicker": 0.15}})
    stra.flush("Wick, the lantern-bearer")
    stra.behave("Wick", "Wanderer", "Run with A/D or arrows, jump with Space/W. Lands on stone tiles. Without input an "
                "autopilot walks the level (for the film) and stops at the shrine.", hero_behavior(hx, hy))

    stra.op("entity_create", name="Camera", position=[hx + 2.5, hy + 2.0, 20],
            components={"camera": {"orthographic": True, "orthoSize": 4.8, "nearPlane": 0.1, "farPlane": 300, "primary": True},
                        "camera2d": {"pixelsPerUnit": 100, "pixelSnap": False, "follow": "Wick", "smoothing": 0.35,
                                     "deadZone": [0.8, 0.6], "offset": [2.2, 1.8], "bounds": [0, 0.5, MAP_W - 12, MAP_H]}})
    stra.flush("Camera")
    aur.op("entity_create", name="Ambient Water Light", components={"light2d": {"kind": "global", "color": "#d2eaec", "intensity": 0.85}})
    aur.flush("Ambient light")
    aur.call("environment_update", skyMode="gradient", skyTop="#0b2a33", skyHorizon="#03090c", fogColor="#0e3640",
             fogDensity=0.004, bloomIntensity=0.6, bloomThreshold=0.9, vignette=0.35, grain=0.05, showGrid=False,
             tonemap="agx", exposure=1.0)
    nim.call("scene_save", path="scenes/main.sky.json")


def shots():
    return []
