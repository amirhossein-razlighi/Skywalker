"""Hidden Alley — a neo-noir night in the rain. Photoscanned CC0 props and textures (Poly
Haven), simulated rain with splashes, puddles that mirror the neon, a burning barrel and a
steaming manhole (volumetric fluid simulations), and light shafts through the wet air."""
import math

import polyhaven as ph
from kit import rnd

META = dict(
    id="hidden_alley", title="Hidden Alley", genre="Neo-noir Stealth", mood="rainy night",
    pitch="Somebody left the lights on. Somebody always does.",
    view=dict(eye=[1.5, 1.7, 16], target=[0, 2.5, -20]),
    assets="Poly Haven (CC0)",
)

HALF = 4.6      # half width of the alley (m)
NEAR, FAR = 18.0, -46.0


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Hidden Alley", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])
    r = rnd(4242)

    # --- Pixel: assets and surfaces (CC0) -----------------------------------------------------
    A = {k: ph.model(pix, k) for k in (
        "street_lamp_01", "street_lamp_02", "metal_trash_can", "trashbag", "barrel_stove", "covered_car",
        "concrete_road_barrier", "utility_box_01", "utility_box_02", "security_light", "exterior_aircon_unit",
        "modular_airduct_circular_01", "fire_hydrant", "street_rat", "water_manhole_cover", "old_tyre",
        "rollershutter_door", "rollershutter_window_01", "security_camera_01", "cardboard_box_01")}
    sky = ph.hdri(pix, "qwantani_night_puresky")
    asphalt = ph.texture(pix, "asphalt_02", tiling=0.35, roughness=0.32)  # rain-slick
    brick = ph.texture(pix, "red_brick_03", tiling=0.5)
    dark_brick = ph.texture(pix, "dark_brick_wall", tiling=0.45)
    plaster = ph.texture(pix, "grey_plaster_02", tiling=0.3)
    concrete = ph.texture(pix, "concrete_floor_worn_001", tiling=0.4, roughness=0.6)
    metal = ph.texture(pix, "rusty_metal_02", tiling=0.8)
    pix.material("materials/window_dark.mat.json", color="#05070a", metallic=0.0, roughness=0.06, clearcoat=1.0)
    for name, col in (("warm", [1.0, 0.62, 0.32, 0.9]), ("warm_dim", [1.0, 0.5, 0.25, 0.35]), ("cool", [0.55, 0.75, 1.0, 0.55]),
                      ("tv", [0.3, 0.5, 1.0, 0.9])):
        pix.material(f"materials/window_{name}.mat.json", color="#000000", emissive=col, roughness=0.3)
    pix.material("materials/frame.mat.json", color="#1a1c20", metallic=0.6, roughness=0.5)
    for name, col in (("pink", [1.0, 0.12, 0.55, 9]), ("cyan", [0.1, 0.85, 1.0, 9]), ("amber", [1.0, 0.55, 0.1, 8])):
        pix.material(f"materials/neon_{name}.mat.json", color="#000000", emissive=col, unlit=True)

    # --- Cirro: the street and its walls -----------------------------------------------------
    length = NEAR - FAR
    mid = (NEAR + FAR) / 2
    cir.e("Street", "cube", pos=(0, -0.1, mid), scale=(HALF * 2 + 2, 0.2, length + 10), material=asphalt, tags=["ground"])
    cir.e("Curb Left", "cube", pos=(-HALF + 0.6, 0.06, mid), scale=(1.2, 0.12, length), material=concrete)
    cir.e("Curb Right", "cube", pos=(HALF - 0.6, 0.06, mid), scale=(1.2, 0.12, length), material=concrete)
    # Facades: blocks of different heights and materials, windows in a grid (some lit).
    z = NEAR
    blocks = []
    while z > FAR:
        w = r.uniform(7, 13)
        blocks.append((z - w / 2, w))
        z -= w
    windows = 0
    for side in (-1, 1):
        for k, (bz, bw) in enumerate(blocks):
            h = r.uniform(11, 22)
            mat = (brick, dark_brick, plaster)[(k + (side > 0)) % 3]
            inset = r.uniform(0, 0.5)
            x = side * (HALF + 0.5 + inset)
            cir.e(f"Building {'LR'[side > 0]}{k + 1}", "cube", pos=(x, h / 2, bz), scale=(1.0, h, bw - 0.05), material=mat,
                  tags=["building"])
            cir.e(f"Cornice {'LR'[side > 0]}{k + 1}", "cube", pos=(x - side * 0.15, h - 0.2, bz), scale=(1.3, 0.35, bw), material=concrete)
            floors = int((h - 3.5) // 3.2)
            cols = max(1, int(bw // 2.6))
            for f in range(floors):
                for c in range(cols):
                    wz = bz - bw / 2 + (c + 0.5) * bw / cols
                    wy = 4.2 + f * 3.2
                    roll = r.random()
                    mat_w = "materials/window_dark.mat.json"
                    if roll < 0.12:
                        mat_w = "materials/window_warm.mat.json"
                    elif roll < 0.22:
                        mat_w = "materials/window_warm_dim.mat.json"
                    elif roll < 0.3:
                        mat_w = "materials/window_cool.mat.json"
                    elif roll < 0.33:
                        mat_w = "materials/window_tv.mat.json"
                    cir.e(f"Window {windows}", "cube", pos=(x - side * 0.52, wy, wz), scale=(0.06, 1.7, 1.1), material=mat_w,
                          castShadows=False, tags=["window"])
                    cir.e(f"Sill {windows}", "cube", pos=(x - side * 0.56, wy - 0.92, wz), scale=(0.18, 0.1, 1.3), material=concrete)
                    cir.e(f"Frame {windows}", "cube", pos=(x - side * 0.5, wy, wz), scale=(0.08, 1.95, 1.35),
                          material="materials/frame.mat.json")
                    cir.e(f"Mullion {windows}", "cube", pos=(x - side * 0.56, wy + 0.2, wz), scale=(0.05, 0.05, 1.1),
                          material="materials/frame.mat.json", castShadows=False)
                    windows += 1
    # Fire escapes: steel landings, railings and stair flights zig-zagging up three facades.
    pix_metal = metal
    for k, (side, zc, floors) in enumerate([(-1, 1.5, 4), (1, -11.5, 3), (-1, -26.0, 4)]):
        fx_x = side * (HALF - 0.55)
        root = f"Fire Escape {k + 1}"
        cir.e(root, pos=(fx_x, 0, zc), tags=["fire_escape"])
        for f in range(floors):
            y = 3.9 + f * 3.2
            cir.e(f"{root} Landing {f + 1}", "cube", pos=(0, y, 0), scale=(1.1, 0.06, 4.2), parent=root, material=pix_metal)
            cir.e(f"{root} Rail {f + 1}", "cube", pos=(-side * 0.53, y + 0.5, 0), scale=(0.04, 0.04, 4.2), parent=root, material=pix_metal)
            for j in range(6):
                cir.e(f"{root} Post {f + 1}.{j + 1}", "cube", pos=(-side * 0.53, y + 0.25, -2.0 + j * 0.8), scale=(0.035, 0.5, 0.035),
                      parent=root, material=pix_metal, castShadows=True)
            if f + 1 < floors:
                cir.e(f"{root} Stairs {f + 1}", "cube", pos=(-side * 0.1, y + 1.6, (0.9 if f % 2 else -0.9)),
                      rot=(48 if f % 2 else -48, 0, 0), scale=(0.65, 0.05, 4.1), parent=root, material=pix_metal)
        cir.e(f"{root} Ladder", "cube", pos=(-side * 0.35, 2.0, 1.6), scale=(0.45, 3.6, 0.04), parent=root, material=pix_metal)
    # Power lines sagging across the alley, with a string of bulbs on one.
    pix.material("materials/cable.mat.json", color="#0a0a0c", roughness=0.6)
    pix.material("materials/bulb.mat.json", color="#000000", emissive=[1.0, 0.75, 0.45, 3.0], unlit=True)
    for k, (zc, y0) in enumerate([(8.5, 9.0), (-3.5, 11.5), (-17.0, 8.2), (-33.0, 10.0)]):
        segs = 10
        for j in range(segs):
            t0, t1 = j / segs, (j + 1) / segs
            x0, x1 = -HALF + t0 * 2 * HALF, -HALF + t1 * 2 * HALF
            sag0, sag1 = 1.2 * 4 * t0 * (1 - t0), 1.2 * 4 * t1 * (1 - t1)
            ya, yb = y0 - sag0, y0 - sag1
            mx, my = (x0 + x1) / 2, (ya + yb) / 2
            seg_len = math.hypot(x1 - x0, yb - ya)
            ang = math.degrees(math.atan2(yb - ya, x1 - x0))
            cir.e(f"Cable {k + 1}.{j + 1}", "cylinder", pos=(mx, my, zc), rot=(0, 0, ang - 90), scale=(0.025, seg_len, 0.025),
                  material="materials/cable.mat.json", castShadows=False)
            if k == 1:
                cir.e(f"Bulb {j + 1}", "sphere", pos=(mx, my - 0.18, zc), scale=0.11, material="materials/bulb.mat.json",
                      castShadows=False, tags=["bulb"])
    cir.e("Dead End", "cube", pos=(0, 8, FAR - 0.5), scale=(HALF * 2 + 3, 16, 1), material=dark_brick, tags=["building"])
    cir.flush("Street, facades and windows")

    # --- Pixel: life along the walls -------------------------------------------------------
    def wall_x(side, off=0.0):
        return side * (HALF - 0.1 - off)

    ph.place(pix, A["rollershutter_door"], "Garage Door", (wall_x(-1, -0.12), 0.0, -8), yaw=90)
    ph.place(pix, A["rollershutter_window_01"], "Shop Shutter", (wall_x(1, -0.12), 0.6, -19), yaw=-90)
    ph.place(pix, A["rollershutter_door"], "Back Door", (wall_x(1, -0.12), 0.0, 4), yaw=-90)
    ph.place(pix, A["covered_car"], "Covered Car", (-2.6, 0.0, -24), yaw=8)
    for k, (x, z, yaw) in enumerate([(-3.7, 6, 90), (3.6, -3, -80), (-3.6, -15, 95)]):
        ph.place(pix, A["metal_trash_can"], f"Dumpster {k + 1}", (x, 0.0, z), yaw=yaw)
    for k in range(9):
        side = -1 if k % 2 else 1
        x, zz = wall_x(side, r.uniform(0.2, 0.6)), r.uniform(FAR + 6, NEAR - 4)
        ph.place(pix, A["trashbag"], f"Trash Bag {k + 1}", (x, 0.0, zz), yaw=r.uniform(0, 360), scale=r.uniform(0.9, 1.3))
    for k in range(5):
        ph.place(pix, A["cardboard_box_01"], f"Box {k + 1}", (-3.4 + r.uniform(-0.3, 0.3), 0.0, 5 + k * 0.5),
                 yaw=r.uniform(0, 360), scale=r.uniform(1.0, 1.6))
    ph.place(pix, A["old_tyre"], "Tyre", (3.4, 0.3, -9), rot=(0, 30, 75))
    ph.place(pix, A["concrete_road_barrier"], "Barrier", (1.5, 0.0, -36), yaw=12)
    ph.place(pix, A["concrete_road_barrier"], "Barrier 2", (-1.0, 0.0, -37.5), yaw=-8)
    ph.place(pix, A["fire_hydrant"], "Hydrant", (3.8, 0.12, 10), yaw=-90)
    ph.place(pix, A["utility_box_01"], "Utility Box", (wall_x(1), 0.12, -13), yaw=-90)
    ph.place(pix, A["utility_box_02"], "Utility Box 2", (wall_x(-1), 0.12, -30), yaw=90)
    for k, (side, y, zz) in enumerate([(-1, 5.2, 0), (1, 8.1, -11), (-1, 11.3, -21), (1, 4.9, -27)]):
        ph.place(pix, A["exterior_aircon_unit"], f"Air Con {k + 1}", (wall_x(side, -0.25), y, zz), yaw=90 if side < 0 else -90)
    ph.place(pix, A["modular_airduct_circular_01"], "Air Duct", (wall_x(1, -0.2), 6.5, -3), yaw=-90)
    ph.place(pix, A["security_camera_01"], "Camera", (wall_x(-1, -0.3), 4.2, -9), yaw=60)
    ph.place(pix, A["water_manhole_cover"], "Manhole", (0.6, 0.01, -12))
    ph.place(pix, A["barrel_stove"], "Barrel", (-3.0, 0.0, -17))
    ph.place(pix, A["street_rat"], "Rat", (2.9, 0.0, -6), yaw=200, scale=1.4)
    pix.flush("Props along the alley")

    # Lamps and the colored glow of the alley.
    for k, zz in enumerate((12, -6, -26)):
        side = 1 if k % 2 == 0 else -1
        ph.place(pix, A["street_lamp_01"], f"Street Lamp {k + 1}", (side * (HALF - 1.5), 0.12, zz), yaw=90 if side > 0 else -90)
        pix.light(f"Lamp Light {k + 1}", (side * (HALF - 1.5), 3.55, zz), "#ffb46a", 16, range_=13, kind="spot",
                  rot=(-90, 0, 0), spot=42, tags=["lamp"])
    for k, (side, zz) in enumerate([(-1, -8), (1, 4), (1, -19)]):
        ph.place(pix, A["security_light"], f"Security Light {k + 1}", (wall_x(side, -0.15), 3.2, zz), yaw=90 if side < 0 else -90)
        pix.light(f"Security Lamp {k + 1}", (wall_x(side, 0.4), 3.0, zz), "#d8e8ff", 10, range_=9, kind="spot", rot=(-90, 0, 0),
                  spot=60)
    # Neon signs: emissive tubes + their light.
    neon = [("pink", -1, 5.5, -1.5, "BAR"), ("cyan", 1, 4.6, -15.5, "HOTEL"), ("amber", -1, 3.8, -28, "OPEN")]
    for k, (col, side, y, zz, label) in enumerate(neon):
        x = wall_x(side, -0.05)
        pix.e(f"Neon {label}", pos=(x, y, zz), tags=["neon"])
        for j in range(3):
            pix.e(f"Neon {label} Tube {j + 1}", "capsule", pos=(0, (j - 1) * 0.42, 0), rot=(90, 0, 0), scale=(0.07, 1.1 - j * 0.15, 0.07),
                  parent=f"Neon {label}", material=f"materials/neon_{col}.mat.json", castShadows=False)
        hexcol = {"pink": "#ff3aa0", "cyan": "#30d8ff", "amber": "#ffa040"}[col]
        pix.light(f"Neon {label} Glow", (x - side * 0.8, y, zz), hexcol, 6, range_=9, tags=["neon_light"])
    pix.flush("Lamps and neon")

    # --- Aurora: rain, wet air, fire and steam --------------------------------------------------
    aur.call("environment_update", skyMode="hdri", hdri=sky, hdriIntensity=0.05, sunElevation=48, sunAzimuth=200,
             sunIntensity=0.55, sunColor="#8ea6e0", sunSize=0.6,
             ambient=0.1, reflections=1.0, fogColor="#0b1018", fogDensity=0.014, fogHeight=0.12, exposure=1.3,
             bloomIntensity=0.35, bloomThreshold=1.6, tonemap="agx", saturation=1.15, contrast=1.12, vignette=0.38,
             temperature=-0.1, ao=1.1, godRays=1.0, haze=0.018, windSpeed=1.2, windDirection=150, showGrid=False)
    aur.call("fx_create", effect="rain", name="Rain", position=[0, 13, -12],
             overrides={"shapeSize": [HALF * 2 + 2, 0, length], "rate": 9000, "maxParticles": 20000, "floorHeight": 0.02,
                        "colorStart": "#d2dcea8c", "colorEnd": "#d2dcea8c", "sizeStart": 0.014, "sizeEnd": 0.014})
    aur.call("fx_create", effect="mist", name="Street Mist", position=[0, 0.3, -12],
             overrides={"shapeSize": [HALF * 2, 0.4, length - 6], "colorStart": "#8a94a830", "rate": 5})
    aur.call("fx_create", effect="burning_barrel", name="Barrel Fire", position=[-3.0, 0.82, -17],
             overrides={"lightRange": 9})
    aur.call("fx_create", effect="steam_vent", name="Manhole Steam", position=[0.6, 0.02, -12])
    # Puddles that mirror the neon.
    for k, (x, zz, size) in enumerate([(-0.8, 8, 3.6), (1.6, -2, 3.0), (-1.6, -10, 4.2), (1.2, -21, 3.4), (-0.4, -31, 3.8),
                                       (0.9, 3, 2.2), (-2.0, -5, 2.4), (2.0, -14, 2.6), (-1.0, -26, 2.8), (0.4, 13, 2.6)]):
        aur.call("fx_create", effect="puddle", name=f"Puddle {k + 1}", position=[x, 0.015, zz], overrides={"size": size})

    # --- Stratus: life ------------------------------------------------------------------------
    for k in range(3):
        stra.behave(f"Neon {neon[k][4]} Glow", "Buzz", "Neon hums; now and then it stutters.", """
on tick
  if chance(0.012) then
    self.light.intensity = 0.6
  else
    self.light.intensity = 6 + sin(time * 50) * 0.2
  end
end""")
    stra.behave("Rat", "Scurry", "Dart along the wall, pause, dart again.", """
on tick
  let phase = time % 6
  if phase < 1.2 then
    self.position = (2.9 + sin(time * 3) * 0.1, 0, -6 - phase * 3.5)
  end
end""")
    stra.behave("Lamp Light 2", "Failing bulb", "The middle lamp flickers like it is about to die.", """
on tick
  if chance(0.04) then
    self.light.intensity = random(2, 8)
  else
    self.light.intensity = 18
  end
end""")
    stra.flush("Neon hum, the rat, a dying lamp")

    nim.e("Player Camera", pos=(1.2, 1.7, 15), rot=(-3, 0, 0), camera={"fov": 55, "primary": True})
    nim.flush("Game camera")
    nim.call("scene_save", path="scenes/main.sky.json")


def shots():
    def push(i, n):  # walk into the alley in the rain
        t = i / (n - 1)
        e = t * t * (3 - 2 * t)
        return dict(eye=[1.4 - 0.6 * e, 1.65, 15 - 13 * e], target=[0.2, 2.6, -24], fov=52)

    def puddle(i, n):  # low over a puddle full of neon
        t = i / (n - 1)
        return dict(eye=[-0.6 + 0.8 * t, 0.35, 0.5 - 2.5 * t], target=[-1.8, 2.5, -14], fov=48)

    def barrel(i, n):  # orbit the burning barrel and the steam
        t = i / (n - 1)
        a = math.radians(-20 + 50 * t)
        return dict(eye=[-3.0 + math.sin(a) * 4.5, 1.5, -17 + math.cos(a) * 4.5], target=[-1.6, 1.4, -15], fov=50)

    def crane(i, n):  # rise above the lamps
        t = i / (n - 1)
        return dict(eye=[0.5, 3 + 9 * t, 10 - 4 * t], target=[0, 0.5 + 1.5 * t, -26], fov=50)

    return [dict(name="push", frames=180, cam=push, warmup=90),
            dict(name="puddle", frames=150, cam=puddle),
            dict(name="barrel", frames=150, cam=barrel),
            dict(name="crane", frames=150, cam=crane)]
