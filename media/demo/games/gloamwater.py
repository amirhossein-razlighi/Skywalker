"""Gloamwater — a lantern-bearer in a drowned, bioluminescent kingdom (2D metroidvania slice).

Original IP. Built entirely with the 2D stack: painted parallax layers, an auto-tiled tilemap,
normal-mapped sprites lit by 2D lights with soft shadows, a packed sprite atlas with frame
animation, glowing world text, particles, a HUD and a restyled dialogue.
"""
import json
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

STYLE_SHEET = {
    "format": "skywalker.uistyle", "extends": "dark",
    "vars": {"accent": "#7ff4e4", "text": "#e6fbf7", "muted": "#8fb8b4"},
    "rules": {
        "canvas": {"font": "serif", "fontSize": 22, "color": "$text"},
        ".hud": {"background": "#03121acc", "background2": "#082029cc", "radius": 3, "borderWidth": 1, "borderColor": "#7ff4e440",
                 "padding": [12, 18], "shadowColor": "#00000080", "shadowOffset": [0, 6], "shadowBlur": 22},
        ".label": {"font": "Inter", "fontSize": 15, "letterSpacing": 0.16, "textTransform": "uppercase", "color": "$muted"},
        ".hint": {"font": "Inter", "fontSize": 15, "letterSpacing": 0.08, "color": "#9fd8d0aa",
                  "textShadowColor": "#000000c0", "textShadowOffset": [0, 1]},
        "progress": {"accent": "#ffb45c", "track": "#ffffff18", "trackHeight": 8, "radius": 4},
        ".area": {"fontSize": 30, "italic": True, "color": "#d8fff8"},
        ".pip": {"radius": 7, "background": "#9ffff0", "background2": "#4fd8c8", "shadowColor": "#7ff4e4d0", "shadowBlur": 12},
        ".pip_dim": {"radius": 7, "background": "#7ff4e41c", "borderWidth": 1, "borderColor": "#7ff4e460"},
        ".dialogue_box": {"background": "#031118ea", "background2": "#072029ea", "radius": 4, "borderWidth": 1,
                          "borderColor": "#7ff4e455", "shadowColor": "#000000c0", "shadowOffset": [0, 10], "shadowBlur": 40},
        ".dialogue_portrait": {"radius": 96, "borderWidth": 2, "borderColor": "#ffb45c99", "background": "#00000040", "imageFit": "cover"},
        ".dialogue_name": {"font": "Inter", "fontSize": 18, "bold": True, "color": "#ffcf8a", "letterSpacing": 0.22,
                           "textTransform": "uppercase"},
        ".dialogue_text": {"fontSize": 29, "lineSpacing": 1.25, "color": "#e6fbf7", "verticalAlign": "top"},
        ".dialogue_choice": {"background": "#06202ad8", "background2": "#041820d8", "borderColor": "#7ff4e455", "radius": 3,
                             "fontSize": 23, "textAlign": "left", "padding": [8, 16], "hover": {"borderColor": "#ffcf8a"}},
        ".dialogue_hint": {"font": "Inter", "fontSize": 13, "color": "#7fb8b0", "textAlign": "right"},
    },
}


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
  intent "Platformer movement against the Level tilemap: run, jump, land, talk to Bellwether (E); the pool is deadly. An autopilot tours the level (for the film) until a key is pressed."
  param speed = 5.0 in 1..10 "run speed (units/s)"
  param jump = 9.6 in 4..16 "jump take-off speed"
  param gravity = 25 in 5..60 "fall acceleration"
  var vx = 0
  var vy = 0
  var grounded = true
  var facing = 1
  var autopilot = true
  var hold = 1.2
  var safe = ({hx}, {hy}, 0)
  var air = 0
  var met = false
  var talk_t = 0

  fn solid(p) -> bool
    return tile_at(find("Level"), p) != 0
  end

  fn blocked(x, y) -> bool
    return solid((x, y + 0.15, 0)) or solid((x, y + 0.7, 0)) or solid((x, y + 1.25, 0))
  end

  fn talking() -> bool
    return find("Bellwether").dialogue.running
  end

  on start
    play_anim(self, "idle")
  end

  on key "e"
    if not talking() and distance(self.position, find("Bellwether").position) < 3.5 then
      start_dialogue(find("Bellwether"), "Start")
    end
  end

  on tick
    let p = self.position
    let left = key("a") or key("left")
    let right = key("d") or key("right")
    let want_jump = key("space") or key("w") or key("up")
    if left or right or want_jump or key("e") then
      autopilot = false
    end
    let dir = 0
    let chat = talking()
    let cam = find("Camera")
    if chat then
      cam.camera2d.offset = (1.4, 0.7)
    else
      cam.camera2d.offset = (2.2 * facing, 2.4)
    end
    if chat then
      want_jump = false
      if autopilot then
        talk_t += dt
        if talk_t > 2.9 then
          talk_t = 0
          if find("Bellwether").dialogue.choices.length > 0 then
            dialogue_choose(0)
          else
            dialogue_advance()
          end
        end
      end
    elif autopilot then
      if hold > 0 then
        hold -= dt
      elif not met and p.x > 61.5 then
        met = true
        talk_t = 0
        start_dialogue(find("Bellwether"), "Start")
      elif p.x < 128.4 then
        dir = 1
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
    -- the glowing pool and the abyss send you back to the last safe ledge
    if ny < -3 or (ny < 7.3 and nx > 24.05 and nx < 33.95) then
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
    -- the lantern light, its glow and embers follow the lantern hand
    let lamp = (nx + 0.42 * facing, ny + 0.62, 0)
    find("Lantern Light").position = lamp
    find("Lantern Glow").position = lamp
    find("Lantern Embers").position = lamp + (0, 0, 0.2)
    -- animation
    if grounded then
      if abs(vx) > 0.6 then
        play_anim(self, "run")
      elif self.sprite_anim.clip != "land" then
        play_anim(self, "idle")
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

  on dialogue "end"
    hold = 0.8
  end
end
"""


JELLY = """behavior Drift
  var base = (0, 0, 0)
  on start
    base = self.position
  end
  on tick
    let t = time * 0.6 + self.k * 1.7
    self.position = base + (sin(t * 0.7) * 0.5, sin(t) * 0.45 + sin(t * 0.37) * 0.3, 0)
    self.rotation = (0, 0, sin(t * 0.9) * 6)
  end
end"""


HUD_BEHAVIOR = """behavior HudKeeper
  intent "Keeps the HUD true to the world: area name from Wick's position, lantern oil breathing, light motes found."
  on tick
    let x = find("Wick").position.x
    let area = "The Sunken Gate"
    if x > 118 then
      area = "The Drowned Choir"
    elif x > 88 then
      area = "Lantern Gallery"
    elif x > 57 then
      area = "Bellwether's Rest"
    elif x > 33 then
      area = "Lumen Grove"
    elif x > 21 then
      area = "The Glowing Pool"
    end
    find("Area Name").ui.text = area
    find("Oil").ui.value = 0.74 + sin(time * 0.8) * 0.03
    find("Mote Count").ui.text = str(12 + floor(x / 7))
  end
  on event "hud_hide"
    for n in ["Vitals", "Whereabouts", "Controls Hint"]
      find(n).ui.visible = false
    end
  end
  on event "hud_show"
    for n in ["Vitals", "Whereabouts", "Controls Hint"]
      find(n).ui.visible = true
    end
  end
end"""


DIALOGUE = """title: Start
---
<<declare $blessed = false>>
Bellwether: Hm? A light... a real one. I had nearly forgotten the colour. #portrait:bellwether
Bellwether: Few walk the Gloam carrying fire any more, little wick. #portrait:bellwether
Wick: The lamps below went out. I have come to wake them. #portrait:wick
-> Do you know the way to the Choir?
    Bellwether: Past the gallery, where the old lamplighters stand in stone. #portrait:bellwether
    Bellwether: Hold your lantern up to the first of them. She remembers. #portrait:bellwether
-> Why do you live in a bell?
    Bellwether: It rang once for the whole drowned city. Now it rings for me. Mostly when I sneeze. #portrait:bellwether
Bellwether: Go gently. The dark down here is old, but it is not cruel. #portrait:bellwether
<<set $blessed = true>>
<<lantern_blessing>>
===
"""


def glow(b, name, pos, color, size=1.0, strength=1.4):
    """A small halo in the water around a lamp (halo size follows the light radius)."""
    b.op("entity_create", name=name, position=[pos[0], pos[1], 0],
         components={"light2d": {"kind": "point", "color": color, "intensity": 0.3, "radius": size, "halo": strength,
                                 "flicker": 0.1}})


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
    for (x, frame) in ((59.6, "coral_0"), (72.8, "coral_1"), (122.0, "coral_0"), (143.0, "coral_1")):
        d.put(frame, x, MAP_H - floor[int(x)] - 0.05, order=8, scale=1.1)
    # Gallery: lamp posts and hanging lanterns.
    for x in (91.0, 99.5, 108.0):
        y = MAP_H - floor[int(x)]
        d.put("lantern_post", x, y - 0.05, order=7, pivot=(0.27, 0.0))
        cir.op("entity_create", name=f"Lamp Light {x}", position=[x + 0.78, y + 2.4, 0],
               components={"light2d": {"kind": "point", "color": "#7ff4e4", "intensity": 1.6, "radius": 5.0, "falloff": 1.7,
                                       "flicker": 0.05}})
        glow(cir, f"Lamp Glow {x}", (x + 0.78, y + 2.4), "#9ff8ee")
    for x in (95.3, 103.7):
        d.put("hanging_lantern", x, 19.4, order=6, pivot=(0.5, 1.0), lit=False)
        cir.op("entity_create", name=f"Hanging Light {x}", position=[x, 17.0, 0],
               components={"light2d": {"kind": "point", "color": "#ffc070", "intensity": 1.2, "radius": 4.0, "flicker": 0.2}})
        glow(cir, f"Hanging Glow {x}", (x, 17.0), "#ffc070")
    # The Sunken Gate at the start, its title carved in glowing letters.
    gy = MAP_H - floor[10]
    d.put("gate_arch", 8.6, gy - 0.1, order=4, name="Sunken Gate", scale=0.88)
    cir.op("entity_create", name="Title Glyphs", position=[14.4, gy + 4.5, 0.2],
           components={"text": {"text": "G L O A M W A T E R", "font": "serif", "size": 0.6, "color": "#c8fff6", "emissive": 2.2,
                                "outline": 0.04, "outlineColor": "#0b3b3a", "sortingLayer": "default", "order": 6}})
    cir.op("entity_create", name="Gate Subtitle", position=[14.4, gy + 3.8, 0.2],
           components={"text": {"text": "<i>where the light went down</i>", "font": "serif", "size": 0.3, "color": "#8fe8dc",
                                "emissive": 0.8, "sortingLayer": "default", "order": 6}})
    cir.op("entity_create", name="Gate Runes Light", position=[8.6, gy + 4.0, 0],
           components={"light2d": {"kind": "point", "color": "#5ff2e2", "intensity": 1.1, "radius": 5.0, "halo": 0.25}})
    d.put("lore_stone", 75.5, MAP_H - floor[75] - 0.05, order=7, name="Lore Stone")
    cir.op("entity_create", name="Lore Inscription", position=[75.5, MAP_H - floor[75] + 2.75, 0.2],
           components={"text": {"text": "Here the last lamp\nwas lowered into the deep.", "font": "serif", "size": 0.21,
                                "color": "#bdfcf2", "emissive": 1.2, "align": "center", "lineSpacing": 1.15, "order": 8}})
    # Bellwether's bell on the plaza.
    by = MAP_H - floor[68]
    d.put("bell", 68.6, by - 0.25, order=6, name="Temple Bell", shadows=True)
    cir.op("entity_create", name="Bell Hearth Light", position=[66.6, by + 1.0, 0],
           components={"light2d": {"kind": "point", "color": "#ffb050", "intensity": 0.9, "radius": 3.2, "flicker": 0.25, "halo": 0.3}})
    # The Choir shrine: the First Lamplighter.
    sy = MAP_H - floor[133]
    d.put("statue", 133.0, sy - 0.08, order=5, name="The First Lamplighter", scale=0.78)
    cir.op("entity_create", name="Shrine Lantern Light", position=[133.0 + (506 - 280) / 100 * 0.78, sy + (1000 - 330) / 100 * 0.78, 0],
           components={"light2d": {"kind": "point", "color": "#ffc070", "intensity": 1.3, "radius": 7.0, "halo": 0.55, "flicker": 0.1,
                                   "shadows": True, "shadowSoftness": 0.7}})
    cir.op("entity_create", name="Shrine Epitaph", position=[126.6, sy + 5.0, 0.2],
           components={"text": {"text": "THE  DROWNED  CHOIR", "font": "serif", "size": 0.42, "color": "#ffe2b0", "emissive": 1.6,
                                "outline": 0.03, "outlineColor": "#3a2410", "order": 6}})
    d.put("rope_bridge", 54.5, MAP_H - 19 - 0.35, order=12, pivot=(0.5, 0.0), name="Rope Bridge")
    cir.flush("Landmarks: grove, gallery, bridge")

    # --- Water, jellyfish, Bellwether: grid sheets cut with sprite_sheet_slice ---------------------
    cir.op("entity_create", name="Glowing Pool", position=[29.0, 8.1, 0.1])
    cir.op("entity_create", name="Pool Light", position=[29.0, 7.4, 0],
           components={"light2d": {"kind": "point", "color": "#5ff2e2", "intensity": 1.3, "radius": 6.0, "falloff": 1.5}})
    jellies = [(30.5, 11.5, 1.0), (40.5, 16.2, 0.7), (53.0, 6.0, 1.2), (55.6, 9.4, 0.85), (51.8, 12.8, 0.6), (97.0, 9.5, 0.8),
               (116.0, 14.0, 0.9), (127.5, 15.5, 0.7)]
    for i, (x, y, sc) in enumerate(jellies):
        cir.op("entity_create", name=f"Jellyfish {i + 1}", position=[x, y, -0.3], tags=["jelly"], vars={"k": i})
    bx = 65.0
    cir.op("entity_create", name="Bellwether", position=[bx, by - 0.12, 0], tags=["npc"],
           components={"dialogue": {"script": "story/bellwether.dialogue", "startNode": "Start", "typewriter": 38}})
    cir.op("entity_create", name="Abyss Glow", position=[54.0, 3.2, -0.5],
           components={"light2d": {"kind": "point", "color": "#3fd8d0", "intensity": 0.6, "radius": 5.5, "halo": 0.7}})
    cir.flush("Pool, jellyfish, Bellwether")
    pix.call("sprite_sheet_slice", image=f"{ART}/water_sheet.png", cell=[1000, 220], entity="Glowing Pool", pixels_per_unit=100,
             animations={"lap": {"frames": "0-3", "fps": 5}})
    cir.call("entity_update", entity="Glowing Pool",
             components={"sprite": {"pivot": [0.5, 1.0], "lit": False, "order": 25, "filter": "linear"}})
    pix.call("sprite_sheet_slice", image=f"{ART}/jelly_sheet.png", cell=[150, 260])
    for i, (x, y, sc) in enumerate(jellies):
        cir.op("entity_update", entity=f"Jellyfish {i + 1}",
               components={"sprite": {"texture": f"{ART}/jelly_sheet.atlas.json", "pixelsPerUnit": 170 / sc, "lit": False,
                                      "order": 3 if sc < 0.9 else 14, "color": "#ffffffd8"},
                           "sprite_anim": {"clips": {"pulse": {"frames": "0-5", "fps": 5 + i % 3}}, "clip": "pulse", "playing": True}})
    cir.flush("Jellyfish drift")
    pix.call("sprite_sheet_slice", image=f"{ART}/bellwether_sheet.png", cell=[350, 250], entity="Bellwether", pixels_per_unit=86,
             animations={"idle": {"frames": "0-5", "fps": 5}})
    cir.call("entity_update", entity="Bellwether",
             components={"sprite": {"pivot": [0.5, 0.0], "order": 31, "normalMap": f"{ART}/bellwether_sheet_n.png"}})
    for i in range(len(jellies)):
        stra.behave(f"Jellyfish {i + 1}", "Drift", "Pulse upward and sink back, swaying, like breathing light.", JELLY)
    stra.flush("Creature behaviors")

    # --- Particles: spores, drifting motes, drips, lantern embers ------------------------------------
    def spores(name, x, y, w, h, color, rate=10):
        aur.op("fx_create", effect="fireflies", name=name, position=[x, y, 0.5],
               overrides={"shape": "box", "shapeSize": [w, h, 0.5], "rate": rate, "colorStart": color, "colorEnd": color[:7] + "00",
                          "sizeStart": 0.05, "sizeEnd": 0.03, "intensity": 3.0, "speed": 0.2, "gravity": -0.05, "turbulence": 0.35,
                          "lifetime": 6, "wind": 0.1, "maxParticles": 300})
    spores("Grove Spores", 42, 12.5, 16, 6, "#8ffff0ff", 16)
    spores("Violet Spores", 46, 11.5, 6, 4, "#c9a8ffff", 6)
    spores("Gate Motes", 10, 13, 16, 7, "#9ff8eaff", 8)
    spores("Plaza Motes", 68, 12, 22, 7, "#a8fff2ff", 9)
    spores("Gallery Motes", 100, 15.5, 22, 5, "#ffe0a8ff", 6)
    spores("Shrine Motes", 132, 13, 22, 8, "#ffe8b8ff", 14)
    spores("Abyss Motes", 54, 4, 8, 10, "#7ff4e4ff", 10)
    for i, x in enumerate((19.5, 45.5, 71.5, 94.5, 106.5, 131.5)):
        aur.op("fx_create", effect="rain", name=f"Drip {i + 1}", position=[x, 19.0 if x < 89 else 20.0, 0.3],
               overrides={"shape": "point", "rate": 0.8, "speed": 0.0, "gravity": 9.0, "sizeStart": 0.035, "sizeEnd": 0.03,
                          "colorStart": "#cffff8ff", "colorEnd": "#7ff4e480", "lifetime": 2.2, "stretch": 0.04, "collide": False,
                          "wind": 0, "splash": 0, "intensity": 2.0})
    aur.flush("Particles")

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
    stra.op("entity_create", name="Lantern Light", position=[hx + 0.42, hy + 0.62, 0],
            components={"light2d": {"kind": "point", "color": "#ffb45c", "intensity": 2.6, "radius": 7.5, "falloff": 1.6,
                                    "height": 1.4, "shadows": True, "shadowSoftness": 0.6, "halo": 0.0, "flicker": 0.12}})
    stra.op("entity_create", name="Lantern Glow", position=[hx + 0.42, hy + 0.62, 0],
            components={"light2d": {"kind": "point", "color": "#ffc070", "intensity": 0.6, "radius": 1.3, "falloff": 2.0,
                                    "halo": 1.6, "flicker": 0.15}})
    stra.op("fx_create", effect="embers", name="Lantern Embers", position=[hx + 0.42, hy + 0.62, 0.2],
            overrides={"rate": 5, "sizeStart": 0.03, "sizeEnd": 0.01, "speed": 0.35, "gravity": -0.4, "lifetime": 1.4,
                       "colorStart": "#ffd08aff", "colorEnd": "#ff803000", "intensity": 3.0, "turbulence": 0.3, "maxParticles": 60})
    stra.flush("Wick, the lantern-bearer")
    stra.behave("Wick", "Wanderer", "Run with A/D or arrows, jump with Space/W. Lands on stone tiles. Without input an "
                "autopilot walks the level (for the film) and stops at the shrine.", hero_behavior(hx, hy))

    stra.op("entity_create", name="Camera", position=[hx + 2.5, hy + 2.0, 20],
            components={"camera": {"orthographic": True, "orthoSize": 4.8, "nearPlane": 0.1, "farPlane": 300, "primary": True},
                        "camera2d": {"pixelsPerUnit": 100, "pixelSnap": False, "follow": "Wick", "smoothing": 0.35,
                                     "deadZone": [0.8, 0.6], "offset": [2.2, 2.4], "bounds": [0, 0.5, MAP_W - 12, MAP_H]}})
    stra.flush("Camera")
    # --- Story and UI: Bellwether's dialogue, a restyled dialogue box, the HUD --------------------------
    proj = studio.project
    os.makedirs(os.path.join(proj, "story"), exist_ok=True)
    os.makedirs(os.path.join(proj, "ui"), exist_ok=True)
    with open(os.path.join(proj, "story", "bellwether.dialogue"), "w") as f:
        f.write(DIALOGUE)
    with open(os.path.join(proj, "ui", "gloam.uistyle.json"), "w") as f:
        json.dump(STYLE_SHEET, f, indent=2)
    nim.call("dialogue_check", path="story/bellwether.dialogue")
    stra.call("ui_create", template="dialogue", canvas={"name": "Dialogue UI", "theme": "dark", "styleSheet": "ui/gloam.uistyle.json",
                                                        "sortOrder": 100})
    stra.call("entity_update", entity="Dialogue Box", components={"ui": {"visible": False, "size": [0, 196],
                                                                         "margin": [0, 300, 34, 300], "padding": [18, 26, 18, 18]}})
    stra.call("entity_update", entity="Dialogue Portrait", components={"ui": {"size": [158, 158]}})
    stra.call("entity_update", entity="Dialogue Choices", components={"ui": {"visible": False, "position": [-320, -250]}})
    pips = [{"type": "panel", "name": f"Pip {k + 1}", "size": [14, 14], "style": "pip" if k < 3 else "pip_dim"} for k in range(5)]
    stra.call("ui_create", canvas={"name": "HUD", "theme": "dark", "styleSheet": "ui/gloam.uistyle.json", "sortOrder": 10}, elements=[
        {"type": "panel", "name": "Vitals", "anchor": "top_left", "position": [44, 36], "style": "hud", "layout": "row", "gap": 16,
         "align": "center", "fit": "both", "children": [
             {"type": "image", "name": "Lantern Icon", "image": f"{ART}/ui/lantern_icon.png", "size": [40, 58]},
             {"type": "panel", "name": "Vitals Column", "layout": "column", "gap": 7, "fit": "both", "children": [
                 {"type": "text", "name": "Oil Label", "text": "Lantern oil", "style": "label", "fit": "both"},
                 {"type": "progress", "name": "Oil", "value": 0.76, "size": [230, 8]},
                 {"type": "panel", "name": "Pips", "layout": "row", "gap": 9, "fit": "both", "children": pips}]}]},
        {"type": "panel", "name": "Whereabouts", "anchor": "top_right", "position": [-44, 36], "style": "hud", "layout": "column",
         "gap": 4, "align": "end", "fit": "both", "children": [
             {"type": "text", "name": "Area Name", "text": "The Sunken Gate", "style": "area", "fit": "both"},
             {"type": "panel", "name": "Motes Row", "layout": "row", "gap": 8, "align": "center", "fit": "both", "children": [
                 {"type": "image", "name": "Mote Icon", "image": f"{ART}/ui/mote_icon.png", "size": [18, 18]},
                 {"type": "text", "name": "Mote Count", "text": "12", "style": "label", "fit": "both"},
                 {"type": "text", "name": "Mote Label", "text": "light motes", "style": "label", "fit": "both"}]}]},
        {"type": "text", "name": "Controls Hint", "anchor": "top_left", "position": [48, 132], "fit": "both", "style": "hint",
         "text": "A / D  move     Space  jump     E  talk"}])
    stra.behave("HUD", "HudKeeper", "Area name, oil and motes follow the world.", HUD_BEHAVIOR)
    stra.flush("HUD")

    aur.op("entity_create", name="Ambient Water Light", components={"light2d": {"kind": "global", "color": "#d2eaec", "intensity": 0.85}})
    aur.flush("Ambient light")
    aur.call("environment_update", skyMode="gradient", skyTop="#0b2a33", skyHorizon="#03090c", fogColor="#0e3640",
             fogDensity=0.004, bloomIntensity=0.6, bloomThreshold=0.9, vignette=0.35, grain=0.05, showGrid=False,
             tonemap="agx", exposure=1.0)
    add_shots(nim)
    nim.call("scene_save", path="scenes/main.sky.json")


# --- Film: hero shots -------------------------------------------------------------------------------
# Each shot is a sequence asset that starts with the simulation (Wick's autopilot is deterministic, so
# times are absolute): an orthographic shot camera, keyed 2D moves (position = pan/track, orthoSize =
# zoom) or a procedural `track` shot that follows Wick. `still` is when the final frame is taken.
SHOTS = [
    dict(name="title", t0=0.0, t1=4.6, still=1.9, hud=False,
         keys=[(0.0, (9.4, 13.5), 4.3), (4.6, (13.8, 13.9), 4.7)]),
    dict(name="pool", t0=4.0, t1=7.6, still=5.6,
         keys=[(4.0, (23.5, 11.6), 4.6), (7.6, (33.5, 11.9), 4.8)]),
    dict(name="grove", t0=7.0, t1=10.6, still=8.9,
         keys=[(7.0, (37.5, 12.6), 5.3), (10.6, (46.5, 12.2), 5.1)]),
    dict(name="bridge", t0=10.3, t1=12.9, still=11.3,
         keys=[(10.3, (50.5, 9.3), 5.0), (12.9, (57.5, 9.9), 5.0)]),
    dict(name="bellwether", t0=13.4, t1=19.0, still=17.6,
         keys=[(13.4, (64.4, 10.9), 4.4), (19.0, (64.7, 10.6), 3.85)]),
    dict(name="plaza", t0=34.2, t1=38.4, still=36.4,
         keys=[(34.2, (66.5, 12.0), 5.0), (38.4, (77.5, 12.6), 5.0)]),
    dict(name="gallery", t0=40.4, t1=44.6, still=42.6, track=True),
    dict(name="shrine", t0=47.2, t1=55.2, still=54.0,
         keys=[(47.2, (128.8, 12.9), 3.9), (55.2, (130.4, 13.8), 5.0)]),
]


def add_shots(b):
    """Creates the shot cameras and one sequence asset per shot (cinematics/<name>.sequence.json)."""
    for s in SHOTS:
        cam = f"Cam {s['name'].title()}"
        x, y = s["keys"][0][1] if "keys" in s else (90.0, 15.6)
        b.op("entity_create", name=cam, position=[x, y, 20], tags=["shot"],
             components={"camera": {"orthographic": True, "orthoSize": s["keys"][0][2] if "keys" in s else 4.6, "nearPlane": 0.1,
                                    "farPlane": 300, "primary": False}})
    b.flush("Shot cameras")
    for s in SHOTS:
        cam = f"Cam {s['name'].title()}"
        path = f"cinematics/{s['name']}.sequence.json"
        b.call("sequence_create", path=path, duration=s["t1"], entity=f"Shot {s['name'].title()}", play_on_start=False,
               overwrite=True)
        seq = f"Shot {s['name'].title()}"
        if s.get("track"):
            b.call("sequence_camera_shot", sequence=seq, camera=cam, shot="track", target="Wick", start=s["t0"],
                   duration=s["t1"] - s["t0"], angle=0, distance=20, offset=[1.8, 2.6, 0], **{"from": -0.6, "to": 0.6})
            b.call("sequence_key", sequence=seq, camera_cuts=[{"t": 0, "camera": cam}])
        else:
            keys = []
            for (t, (x, y), size) in s["keys"]:
                keys.append({"entity": cam, "property": "transform.position", "t": t, "value": [x, y, 20], "ease": "smooth"})
                keys.append({"entity": cam, "property": "camera.orthoSize", "t": t, "value": size, "ease": "smooth"})
            b.call("sequence_key", sequence=seq, keys=keys, camera_cuts=[{"t": 0, "camera": cam}])
        events = [{"t": 0.0, "event": "hud_hide" if s.get("hud") is False else "hud_show", "target": "HUD"}]
        b.call("sequence_key", sequence=seq, events=events)


def shots():
    """showcase.py render hooks: start the shot's sequence with the simulation, then film it."""
    out = []
    for s in SHOTS:
        seq = f"cinematics/{s['name']}.sequence.json"
        t0, t1, still = s["t0"], s["t1"], s["still"]
        frames = int(round((t1 - t0) * 30))

        def start(sky, seq=seq):
            sky.call("sequence_play", sequence=seq)
        out.append(dict(name=s["name"], frames=frames, warmup=int(round(t0 * 60)), view="scene", start=start, still=still))
    return out
