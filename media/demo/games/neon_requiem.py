"""Neon Requiem — a rain-soaked megacity street at night (original IP; benchmark: modern AAA
cyberpunk open worlds).

Halcyon Avenue cuts through the Kessler Ward: a canyon of Blender-modeled tenements, capsule
megablocks and glass towers (window grids, balconies, AC units, pipes, fire escapes, rooftop
tanks), hundreds of neon signs that really light the street, skybridges, a monorail over the
Ninth Street crossing, wet asphalt and puddles mirroring the neon, rain with splashes, steam
vents, drones, flying traffic, a holographic giant and a distant skyline of megatowers.

Everything is original: the city, the brands (Lumen 24, Noodle Orbit, Hotel Vesper, Vanta
Helix, Aether-9, ...) and the invented glyph script on the blade signs. Photoscanned props and
surfaces are Poly Haven (CC0); the walker is Khronos' CesiumMan (CC-BY 4.0).

Build:  python3 media/demo/showcase.py build neon_requiem
"""
import hashlib
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import polyhaven as ph  # noqa: E402
from kit import rnd  # noqa: E402

META = dict(
    id="neon_requiem", title="Neon Requiem", genre="Open-world Neo-noir RPG", mood="rain, neon, midnight",
    pitch="Halcyon Avenue never sleeps. It just stops remembering.",
    view=dict(eye=[2.5, 1.7, 30], target=[0, 9, -60]),
    assets="Poly Haven (CC0), Khronos glTF sample CesiumMan (CC-BY 4.0), Blender-generated city kit",
)

HERE = os.path.dirname(os.path.abspath(__file__))
FACADE = 10.0                 # facade line on both sides of the avenue (x = +-10)
ROAD = 6.5                    # half width of the road (curb at x = +-6.5)
NEAR, FAR = 46.0, -330.0      # avenue extent (z)
CROSS = (-84.0, -70.0)        # Ninth Street crossing (z range)
BRIDGES = [(-40.0, 31.0, "led_cyan"), (-132.0, 47.0, "led_magenta")]   # (z, deck height, accent)
RAIL_Z, RAIL_H = -77.0, 21.0  # monorail over the crossing
KIT = "generated/kit"
SIGNS = "generated/signs"
ADS = [dict(name="aether", headline="AETHER-9", tagline="TASTE  THE  STATIC", colors=["ff2a8a", "ff9a1a"], frames=12),
       dict(name="vanta", headline="VANTA HELIX", tagline="WE  BUILD  TOMORROW", colors=["1a3aff", "28e0ff"], frames=12, emblem="helix"),
       dict(name="synthia", headline="SYNTHIA", tagline="BE  ANYONE", colors=["1ad8ff", "a24bff"], frames=12, res=[384, 768],
            emblem="helix"),
       dict(name="kairos", headline="KAIROS AIR", tagline="FLY  ABOVE  THE  RAIN", colors=["ff6a1a", "ffd36b"], frames=12),
       dict(name="lumen", headline="LUMEN 24", tagline="ALWAYS  OPEN", colors=["3dff8c", "f4f0ff"], frames=12, res=[384, 768])]

# Invented brands and signage (no real companies, no real script)
SHOP_NAMES = ["LUMEN 24", "PHARMA+", "SOLACE CLINIC", "BYTE BAZAAR", "RAMEN 9", "NEON DOLL", "ECHO LOUNGE",
              "MERIDIAN BAR", "ZEPHYR NOODLE", "IRIS OPTICS", "VOLTA CELL", "DREAMLINE", "KESSLER PAWN",
              "ORBIT TEA", "NOX CLUB", "SYNAPSE REPAIR", "HALO DELI", "CHROME & SOUL", "OXIDE TATTOO",
              "QUANTA DUMPLING", "MOTEL ASTRA", "24H LAUNDRY", "VANTA CREDIT", "GLITCH ARCADE", "KARAOKE",
              "MEMORY PARLOR", "LOTUS TEA", "AURA BEAUTY", "RAINCHECK BAR", "ICHOR LABS", "NOODLE ORBIT",
              "HOTEL VESPER", "CYAN DINER", "STATIC RECORDS", "LUCKY CIRCUIT", "MIDNIGHT MART"]
NEON_WORDS = ["HOTEL VESPER", "NOODLE ORBIT", "GLITCH ARCADE", "BAR", "OPEN", "LIVE", "REQUIEM", "KESSLER WARD",
              "24H", "RAMEN", "KARAOKE", "MOTEL", "DREAMS", "NO VACANCY", "LOVE", "PAWN"]
NEON = {"pink": "ff2a8a", "cyan": "28e0ff", "amber": "ffa31a", "violet": "a24bff", "red": "ff2a2a",
        "green": "3dff8c", "white": "f4f0ff", "orange": "ff6a1a", "blue": "3a6bff"}
PANELS = [("f4f1ea", "101014"), ("ffe9a8", "1a1208"), ("9ff3ff", "04161a"), ("ff9ccc", "1a0610"),
          ("f0f0f0", "c41e3a"), ("1a1a1e", "28e0ff"), ("1a1a1e", "ff2a8a"), ("ffd36b", "101014"), ("c8ffdf", "05140c")]

# Photoscanned surfaces (Poly Haven, CC0) per building material slot
WALLS = {
    "wall_plaster": ["painted_plaster_wall", "plastered_wall_05", "grey_plaster_03", "beige_wall_002"],
    "wall_concrete": ["preconcrete_wall_001", "concrete_wall_006", "concrete_panels", "concrete_wall_008"],
    "wall_tile": ["rectangular_facade_tiles", "rectangular_facade_tiles_02", "concrete_tile_facade"],
    "wall_brick": ["brick_wall_08", "painted_worn_brick", "brick_4"],
    "wall_panel": ["metal_plate", "factory_wall", "box_profile_metal_sheet"],
}
OTHER_SURF = {"trim": "concrete_wall_003", "concrete_raw": "concrete_floor_worn_001", "shutter": "painted_metal_shutter",
              "metal_rust": "rusty_metal_02", "ac_body": "metal_plate_02"}


def hexrgb(h):
    return [int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)]


# =============================================================================================
# Layout (deterministic): which buildings stand where
# =============================================================================================
def layout():
    r = rnd(909)
    blds = []
    styles_main = ["tenement", "tenement", "megablock", "brick", "tile", "glass", "megablock", "tenement"]

    def need_height(z0, z1):
        h = 0
        for bz, bh, _ in BRIDGES:
            if z1 - 2 <= bz <= z0 + 2:
                h = max(h, bh + 10)
        return h

    for side in (-1, 1):
        z = NEAR
        k = 0
        while z > FAR:
            if CROSS[0] < z <= CROSS[1] + 0.01:
                z = CROSS[0]
                continue
            w = r.uniform(14, 24)
            if z - w < CROSS[1] and z > CROSS[1]:
                w = z - CROSS[1]  # end flush at the crossing
                if w < 9:
                    z = CROSS[0]
                    continue
            z0, z1 = z, z - w
            style = r.choice(styles_main)
            tall = r.random() < 0.3
            h = r.uniform(85, 150) if tall else r.uniform(34, 72)
            h = max(h, need_height(z0, z1))
            if z0 > 20:
                h = min(h, r.uniform(32, 48))  # keep the sky open over the first block
            depth = r.uniform(18, 26)
            name = f"bld_{'l' if side < 0 else 'r'}{k:02d}"
            spec = dict(name=name, width=round(w - 0.15, 2), depth=round(depth, 2), height=round(h, 1), style=style,
                        side_style=("glass" if style == "glass" else r.choice(["tenement", "megablock", "tile"])),
                        seed=r.randint(1, 10_000), ground=round(r.uniform(4.6, 5.6), 2), floor=round(r.uniform(3.1, 3.5), 2),
                        lit=round(r.uniform(0.3, 0.45), 2))
            if tall and h > 70:
                spec["setback_at"] = round(h * r.uniform(0.45, 0.65), 1)
                spec["setback"] = round(r.uniform(2.0, 3.5), 1)
            zc = (z0 + z1) / 2
            blds.append(dict(spec=spec, pos=[side * FACADE, 0.0, zc], yaw=90 if side < 0 else -90, side=side, z0=z0, z1=z1,
                             row="main"))
            z = z1
            k += 1
    # Ninth Street rows (facing the crossing) and a second row behind the avenue
    for qx in (-1, 1):
        for qz, yaw, face_z in ((1, 180, CROSS[1]), (-1, 0, CROSS[0])):
            x = qx * 40
            k = 0
            while abs(x) < 170:
                w = r.uniform(16, 26)
                xc = x + qx * w / 2
                h = r.uniform(40, 110)
                spec = dict(name=f"bld_x{'l' if qx < 0 else 'r'}{'n' if qz > 0 else 's'}{k}", width=round(w - 0.2, 2),
                            depth=22.0, height=round(h, 1), style=r.choice(styles_main), side_style="tenement",
                            seed=r.randint(1, 10_000), ground=5.0, floor=3.3, lit=0.34, detail=0.6)
                blds.append(dict(spec=spec, pos=[xc, 0.0, face_z], yaw=yaw, side=0, row="cross"))
                x += qx * w
                k += 1
    for side in (-1, 1):
        z = 30.0
        k = 0
        while z > -320:
            w = r.uniform(22, 34)
            if CROSS[0] - 6 < z - w / 2 < CROSS[1] + 6:
                z -= w
                continue
            h = r.uniform(70, 190)
            spec = dict(name=f"bld_b{'l' if side < 0 else 'r'}{k:02d}", width=round(w - 1, 2), depth=26.0, height=round(h, 1),
                        style=r.choice(["glass", "megablock", "tile", "glass"]), side_style="glass", seed=r.randint(1, 9999),
                        ground=6.0, floor=3.4, lit=0.3, detail=0.3, setback_at=round(h * 0.6, 1), setback=3.0)
            blds.append(dict(spec=spec, pos=[side * (FACADE + 34 + r.uniform(0, 8)), 0.0, z - w / 2], yaw=90 if side < 0 else -90,
                             side=side, row="back"))
            z -= w + r.uniform(0, 6)
            k += 1
    return blds


def skyline():
    r = rnd(4242)
    out = []
    for k in range(26):
        z = r.uniform(-1100, -420)
        x = r.uniform(-520, 520)
        if abs(x) < 60 and z > -600:
            x += 140 * (1 if x >= 0 else -1)
        h = r.uniform(220, 520)
        out.append(dict(name=f"sky_t{k:02d}", width=round(r.uniform(40, 80), 1), depth=round(r.uniform(40, 80), 1), height=round(h, 1),
                        tiers=r.choice([1, 2, 3, 3]), seed=r.randint(1, 9999),
                        crown=r.choice(["led_cyan", "led_magenta", "led_amber", "led_white", "led_red"]), pos=[x, 0, z],
                        yaw=r.uniform(-20, 20)))
    return out


def to_world(b, p):
    """Blender-local point of a placed model -> engine world."""
    x, y, z = p
    gx, gy, gz = x, z, -y
    t = math.radians(b["yaw"])
    X = gx * math.cos(t) + gz * math.sin(t)
    Z = -gx * math.sin(t) + gz * math.cos(t)
    return [b["pos"][0] + X, b["pos"][1] + gy, b["pos"][2] + Z]


def dir_world(b, n):
    x, y, z = n
    gx, gy, gz = x, z, -y
    t = math.radians(b["yaw"])
    return [gx * math.cos(t) + gz * math.sin(t), gy, -gx * math.sin(t) + gz * math.cos(t)]


# =============================================================================================
# The kit: one Blender run builds every model (cached by a hash of the script + job)
# =============================================================================================
def signed(b):
    """Blocks that get signs and shop light: the avenue and the near part of Ninth Street."""
    return b["row"] == "main" or (b["row"] == "cross" and abs(b["pos"][0]) < 100)


def make_signs(blds, anchors):
    """Sign specs for every anchor on the avenue (unique text, sized to its shop)."""
    r = rnd(77)
    signs, placed = [], []
    names = list(SHOP_NAMES)
    r.shuffle(names)
    ni = 0
    # big animated screens: the four tallest facade spots between the first block and the second bridge
    cands = []
    for b in blds:
        if b["row"] == "main" and -175 < (b["z0"] + b["z1"]) / 2 < -8:
            for k, a in enumerate(anchors.get(b["spec"]["name"], [])):
                if a["kind"] == "billboard" and a["side"] == "front" and a["pos"][2] > 14:
                    cands.append((b["spec"]["height"], b["spec"]["name"], k, b, a))
    cands.sort(key=lambda c: -c[0])
    screens, used, sides = [], set(), {}
    for h, bn, k, b, a in cands:
        if len(screens) >= 4 or bn in used or sides.get(b["side"], 0) >= 2:
            continue
        used.add(bn)
        sides[b["side"]] = sides.get(b["side"], 0) + 1
        vertical = len(screens) % 2 == 0
        w, hh = (7.0, 14.0) if vertical else (16.0, 8.0)
        name = f"sg_screen_{len(screens)}"
        signs.append(dict(name=name, kind="billboard", width=w, height=hh))
        ad = ["synthia", "aether", "lumen", "vanta"][len(screens)] if vertical else ["aether", "vanta", "kairos"][len(screens) // 2]
        placed.append(dict(model=name, building=b, anchor=a, light="ffffff", kind="screen", w=w, h=hh, ad=ad, vertical=vertical))
        screens.append((bn, k))
    for b in blds:
        if not signed(b):
            continue
        for k, a in enumerate(anchors.get(b["spec"]["name"], [])):
            if (b["spec"]["name"], k) in screens:
                continue
            if a["kind"] == "shop_sign" and a["side"] == "front":
                name = f"sg_{b['spec']['name']}_{k}"
                text = names[ni % len(names)]
                ni += 1
                if r.random() < 0.35 and len(text) < 13:
                    col = r.choice(list(NEON.values()))
                    spec = dict(name=name, kind="neon_text", text=text, size=0.85, color=col, strength=round(r.uniform(5, 8), 1),
                                max_width=a["width"] - 0.4, tube=0.03)
                    light = col
                else:
                    panel, txt = r.choice(PANELS)
                    spec = dict(name=name, kind="lightbox", text=text, width=round(a["width"] - 0.2, 2), height=round(min(1.0, a["height"]), 2),
                                panel=panel, panel_strength=round(r.uniform(1.6, 3.0), 1), text_color=txt,
                                text_strength=(5.0 if panel == "1a1a1e" else 0))
                    light = txt if panel == "1a1a1e" else panel
                signs.append(spec)
                placed.append(dict(model=name, building=b, anchor=a, light=light, kind="shop"))
            elif a["kind"] == "blade" and a["side"] == "front" and r.random() < 0.75:
                name = f"sg_{b['spec']['name']}_{k}"
                col = r.choice(list(NEON.values()))
                spec = dict(name=name, kind="blade", height=round(r.uniform(4, 9), 1), width=round(r.uniform(0.9, 1.3), 2),
                            seed=r.randint(1, 9999), color=col, strength=round(r.uniform(5, 9), 1),
                            rim=r.choice(["ffffff", col, "ffd080"]))
                signs.append(spec)
                placed.append(dict(model=name, building=b, anchor=a, light=col, kind="blade"))
            elif a["kind"] == "billboard" and a["side"] == "front" and r.random() < 0.6:
                name = f"sg_{b['spec']['name']}_{k}"
                text = r.choice(NEON_WORDS)
                col = r.choice(list(NEON.values()))
                spec = dict(name=name, kind="neon_text", text=text, size=r.uniform(1.8, 3.2), color=col,
                            strength=round(r.uniform(6, 9), 1), max_width=min(14, a["width"] * 0.7), tube=0.06)
                signs.append(spec)
                placed.append(dict(model=name, building=b, anchor=a, light=col, kind="facade"))
            elif a["kind"] == "roof_sign" and r.random() < 0.5:
                name = f"sg_{b['spec']['name']}_{k}"
                col = r.choice(list(NEON.values()))
                text = r.choice(["VANTA HELIX", "AETHER-9", "SYNTHIA", "KAIROS", "HALCYON", "IRIS", "NOX"])
                signs.append(dict(name=name, kind="roof_text", text=text, size=4.0, color=col, strength=7.0))
                placed.append(dict(model=name, building=b, anchor=a, light=col, kind="roof"))
    return signs, placed


def run_dcc(pix, job, folder, label):
    """Runs a Blender job (only if the script or job changed since the last run) and imports every
    model it lists; returns ({name: import result}, anchors)."""
    with open(os.path.join(HERE, "neon_requiem_dcc.py")) as f:
        script = f.read()
    blob = json.dumps(job, sort_keys=True)
    digest = hashlib.sha256((script + blob).encode()).hexdigest()[:16]
    stamp = os.path.join(pix.studio.project, folder, ".job")
    fresh = os.path.exists(stamp) and open(stamp).read().strip() == digest
    imported = {}
    if not fresh:
        res = pix.call("dcc_run_script", script=script, args=[blob], name=label, out_dir=folder, timeout_s=3600, **{"import": True},
                       description=f"Neon Requiem {label} (procedural, Blender)", tags=["neon_requiem", "city"])
        for item in res.get("imported", []) if isinstance(res, dict) else []:
            imported[os.path.splitext(os.path.basename(item["file"]))[0]] = item
        with open(stamp, "w") as f:
            f.write(digest)
    names = [b["name"] for b in job.get("buildings", [])] + [t["name"] for t in job.get("skyline", [])]
    for group in ("skybridges", "monorail", "signs", "props"):
        names += [s["name"] for s in job.get(group, [])]
    for n in names:
        if n not in imported:
            imported[n] = pix.call("asset_import", path=f"{folder}/{n}.glb", normalize=False)
    with open(os.path.join(pix.studio.project, folder, "anchors.json")) as f:
        anchors = json.load(f)
    return imported, anchors


def kit_job_extra():
    return dict(
        skybridges=[dict(name=f"skybridge_{i}", length=2 * FACADE + 1.0, width=4.2, height=3.4, accent=acc)
                    for i, (_, _, acc) in enumerate(BRIDGES)],
        monorail=[dict(name="rail_track", kind="track", length=360, height=RAIL_H, piers=[-150, -110, -70, -30, 30, 70, 110, 150]),
                  dict(name="rail_car_head", length=15, nose=-1, stripe="led_magenta"),
                  dict(name="rail_car_mid", length=15, nose=0, stripe="led_magenta"),
                  dict(name="rail_car_tail", length=15, nose=1, stripe="led_magenta")],
        props=[dict(name="prop_street_light", kind="street_light"), dict(name="prop_bollard", kind="bollard"),
               dict(name="prop_noodle_stall", kind="noodle_stall"), dict(name="prop_drone", kind="drone"),
               dict(name="prop_steam_grate", kind="steam_grate"),
               dict(name="prop_hover_car_a", kind="hover_car", under="led_cyan"),
               dict(name="prop_hover_car_b", kind="hover_car", under="led_magenta"),
               dict(name="prop_lantern", kind="paper_lantern"),
               dict(name="prop_holo_ring_0", kind="holo_ring", radius=3.6, thickness=0.06),
               dict(name="prop_holo_ring_1", kind="holo_ring", radius=2.4, thickness=0.04, mat="led_white"),
               dict(name="prop_holo_ring_2", kind="holo_ring", radius=4.4, thickness=0.03),
               dict(name="prop_umbrella_cyan", kind="umbrella", shaft="led_cyan"),
               dict(name="prop_umbrella_pink", kind="umbrella", shaft="led_magenta"),
               dict(name="prop_umbrella_amber", kind="umbrella", shaft="led_amber")],
    )


# =============================================================================================
# Build
# =============================================================================================
def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Neon Requiem", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])
    r = rnd(2077)

    # --- Pixel: photoscanned surfaces (Poly Haven, CC0) ---------------------------------------
    surf = {}
    for slot, ids in WALLS.items():
        for tid in ids:
            surf[tid] = ph.texture(pix, tid, tiling=0.5)
    for tid in OTHER_SURF.values():
        surf[tid] = ph.texture(pix, tid, tiling=0.6)
    asphalt = ph.texture(pix, "asphalt_02", tiling=0.3, roughness=0.26)
    paving = ph.texture(pix, "concrete_pavers_02", tiling=0.55, roughness=0.5)
    curb = ph.texture(pix, "concrete_floor_worn_001", tiling=0.6, roughness=0.55)

    # --- Pixel: the Blender city kit ------------------------------------------------------------
    blds = layout()
    towers = skyline()
    # pass 1: the city kit (buildings report anchors: where signs, lamps and beacons go)
    kit_job = dict(textures=True, buildings=[b["spec"] for b in blds],
                   skyline=[{k: v for k, v in t.items() if k not in ("pos", "yaw")} for t in towers], **kit_job_extra())
    models, anchors = run_dcc(pix, kit_job, KIT, "kit")
    # pass 2: a sign for every anchor (unique text, sized to its shop) and the animated ads
    signs, placed_signs = make_signs(blds, anchors)
    sign_models, _ = run_dcc(pix, dict(textures=False, signs=signs, ads=ADS), SIGNS, "signs")
    models.update(sign_models)

    # bind photoscans and the window atlas to every building's material slots
    atlas = f"{KIT}/window_atlas.png"
    sky_atlas = f"{KIT}/skyline_atlas.png"
    for b in blds:
        name = b["spec"]["name"]
        rr = rnd(b["spec"]["seed"])
        tint = rr.uniform(0.75, 1.05)
        for part in models[name].get("parts", []):
            pn, mp = part["name"], part["material"]
            if pn in WALLS:
                src = surf[rr.choice(WALLS[pn])]
                restyle(pix, mp, src, tint=tint)
            elif pn in OTHER_SURF:
                restyle(pix, mp, surf[OTHER_SURF[pn]], tint=0.9)
            elif pn == "windows":
                pix.material(mp, color="#ffffff", texture=atlas, emissiveMap=atlas, emissive=[1, 1, 1, 1.5], roughness=0.06,
                             metallic=0.0, ormMap="", normalMap="", triplanar=False)
    for t in towers:
        for part in models[t["name"]].get("parts", []):
            if part["name"] == "skyline_windows":
                pix.material(part["material"], color="#ffffff", texture=sky_atlas, emissiveMap=sky_atlas, emissive=[1, 1, 1, 3.0],
                             roughness=0.3, triplanar=False)

    # --- Cirro: the avenue ------------------------------------------------------------------------
    length = NEAR - FAR
    mid = (NEAR + FAR) / 2
    cir.e("Avenue", "cube", pos=(0, -0.25, mid), scale=(2 * ROAD, 0.5, length + 40), material=asphalt, tags=["ground"])
    cir.e("Ninth Street", "cube", pos=(0, -0.251, sum(CROSS) / 2), scale=(400, 0.5, CROSS[1] - CROSS[0]), material=asphalt, tags=["ground"])
    for side in (-1, 1):
        for zz0, zz1 in ((NEAR + 20, CROSS[1]), (CROSS[0], FAR - 20)):
            zc, ln = (zz0 + zz1) / 2, zz0 - zz1
            sx = side * (ROAD + FACADE) / 2
            cir.e(f"Sidewalk {side} {int(zc)}", "cube", pos=(sx, -0.1 + 0.075, zc), scale=(FACADE - ROAD + 1.0, 0.35, ln),
                  material=paving, tags=["ground"])
            cir.e(f"Curb {side} {int(zc)}", "cube", pos=(side * (ROAD + 0.12), 0.0, zc), scale=(0.24, 0.32, ln), material=curb)
    # cross-street sidewalks
    for qx in (-1, 1):
        for zz in (CROSS[1] - 1.75, CROSS[0] + 1.75):
            cir.e(f"Cross Sidewalk {qx} {int(zz)}", "cube", pos=(qx * 105, -0.025, zz), scale=(190, 0.35, 3.5), material=paving, tags=["ground"])
    cir.flush("Avenue, sidewalks, curbs")

    # road markings: dashed center line, lane edges, crosswalks
    pix.material("materials/road_paint.mat.json", color="#d8d4c8", roughness=0.35, metallic=0.0)
    pix.material("materials/road_paint_yellow.mat.json", color="#d8a020", roughness=0.35)
    z = NEAR
    k = 0
    while z > FAR:
        if not (CROSS[0] - 6 < z < CROSS[1] + 6):
            cir.e(f"Lane Dash {k}", "cube", pos=(0, 0.003, z), scale=(0.15, 0.01, 3.0), material="materials/road_paint_yellow.mat.json",
                  castShadows=False, tags=["marking"])
            k += 1
        z -= 7.5
    for s in (-1, 1):
        for zz0, zz1 in ((NEAR, CROSS[1] + 5), (CROSS[0] - 5, FAR)):
            cir.e(f"Edge Line {s} {int(zz0)}", "cube", pos=(s * (ROAD - 0.4), 0.003, (zz0 + zz1) / 2), scale=(0.12, 0.01, zz0 - zz1),
                  material="materials/road_paint.mat.json", castShadows=False, tags=["marking"])
    for zz in (CROSS[1] + 3.0, CROSS[0] - 3.0):
        for i in range(12):
            cir.e(f"Zebra {int(zz)} {i}", "cube", pos=(-ROAD + 0.8 + i * 1.08, 0.003, zz), scale=(0.55, 0.01, 3.6),
                  material="materials/road_paint.mat.json", castShadows=False, tags=["marking"])
    cir.flush("Road markings")

    # buildings
    for b in blds:
        ph.place(cir, models[b["spec"]["name"]], b["spec"]["name"], b["pos"], yaw=b["yaw"])
    cir.flush("City blocks (Blender kit)")
    for t in towers:
        ph.place(cir, models[t["name"]], t["name"], t["pos"], yaw=t["yaw"])
    cir.flush("Skyline megatowers")
    for i, (bz, bh, _) in enumerate(BRIDGES):
        ph.place(cir, models[f"skybridge_{i}"], f"Skybridge {i + 1}", (0, bh, bz), yaw=0)
    ph.place(cir, models["rail_track"], "Monorail Track", (0, 0, RAIL_Z), yaw=0)
    cir.flush("Skybridges and the monorail")

    # --- Aurora: signs and the light they cast -------------------------------------------------------
    nlights = 0
    screens = []
    for k, s in enumerate(placed_signs):
        b, a = s["building"], s["anchor"]
        p = to_world(b, a["pos"])
        n = dir_world(b, a.get("normal", [0, -1, 0]))
        ph.place(aur, models[s["model"]], f"Sign {k}", p, yaw=b["yaw"], tags=["sign", s["kind"]])
        if s["kind"] == "screen":
            screen = f"Ad Screen {len(screens)}"
            sp = [p[0] + n[0] * 0.63, p[1], p[2] + n[2] * 0.63]
            frame0 = f"{SIGNS}/ad_{s['ad']}_00.png"
            aur.e(screen, "quad", "#000000", sp, rot=(0, b["yaw"], 0), scale=(s["w"], s["h"], 1), emissive=[1, 1, 1, 2.6],
                  emissiveMap=frame0, roughness=0.25, castShadows=False, tags=["ad_screen"],
                  vars={"ads": (list(dict.fromkeys([s["ad"], "synthia", "lumen"])) if s["vertical"]
                                else list(dict.fromkeys([s["ad"], "kairos", "vanta", "aether"]))),
                        "shown": ""})
            ad_col = {"aether": "#ff4a8a", "vanta": "#3a7bff", "synthia": "#5ad8ff", "kairos": "#ff8a3a", "lumen": "#7affb0"}[s["ad"]]
            aur.light(f"{screen} Spill", [sp[0] + n[0] * 5, sp[1] - s["h"] * 0.2, sp[2] + n[2] * 5], ad_col, 16, 30)
            screens.append(screen)
            continue
        col = "#" + s["light"]
        off = {"shop": 0.9, "blade": 1.3, "facade": 1.6, "roof": 3.0}[s["kind"]]
        lp = [p[0] + n[0] * off, p[1] + (0 if s["kind"] != "roof" else 3), p[2] + n[2] * off]
        inten = {"shop": 5.0, "blade": 10.0, "facade": 22.0, "roof": 30.0}[s["kind"]]
        rng_ = {"shop": 9.0, "blade": 13.0, "facade": 24.0, "roof": 30.0}[s["kind"]]
        aur.light(f"Sign {k} Light", lp, col, inten, rng_, tags=["sign_light"])
        nlights += 1
    aur.flush(f"{len(placed_signs)} signs")
    # shop interiors spill onto the sidewalk
    for b in blds:
        if not signed(b):
            continue
        for a in anchors.get(b["spec"]["name"], []):
            if a["kind"] == "shop_light" and a["side"] == "front":
                p = to_world(b, a["pos"])
                aur.light(f"Shop Spill {nlights}", p, r.choice(["#ffd2a0", "#e8f0ff", "#ffe6c0"]), 3.5, 8.0, tags=["shop_light"])
                nlights += 1
    aur.flush("Shop light spill")

    # street lights
    z = NEAR - 4
    k = 0
    while z > FAR + 10:
        if not (CROSS[0] - 4 < z < CROSS[1] + 4):
            side = 1 if k % 2 == 0 else -1
            x = side * (ROAD + 0.6)
            ph.place(cir, models["prop_street_light"], f"Street Light {k}", (x, 0.0, z), yaw=-90 if side > 0 else 90)
            head = (x - side * 3.0, 8.75, z)
            aur.light(f"Street Lamp {k}", head, "#ffd2a8" if k % 3 else "#d8e6ff", 40, 22, kind="spot", rot=(-90, 0, 0), spot=70,
                      tags=["street_lamp"])
            k += 1
        z -= 13
    for qx in (-1, 1):
        for i, x in enumerate(range(18, 100, 14)):
            zz = CROSS[1] - 0.6 if i % 2 == 0 else CROSS[0] + 0.6
            sd = -1 if zz > RAIL_Z else 1   # arm points into the street
            ph.place(cir, models["prop_street_light"], f"Cross Light {qx} {i}", (qx * x, 0.15, zz), yaw=0 if sd > 0 else 180)
            aur.light(f"Cross Lamp {qx} {i}", (qx * x, 8.9, zz + sd * 3.0), "#ffd2a8" if i % 3 else "#d8e6ff", 40, 22, kind="spot",
                      rot=(-90, 0, 0), spot=70, tags=["street_lamp"])
    cir.flush("Street lights")
    aur.flush("Street lamps")

    for screen in screens:
        stra.behave(screen, "Ad Loop", "Play the animated ads in turn: 12 frames at 10 fps, a new ad every 7 seconds.", f"""
var ads: list = []
var shown = ""
on tick
  let i = floor(time / 7) % ads.length
  let f = floor(time * 10) % 12
  let pad = "0"
  if f >= 10 then
    pad = ""
  end
  let path = "{SIGNS}/ad_" + ads[i] + "_" + pad + f + ".png"
  if path != shown then
    shown = path
    self.mesh.emissiveMap = path
  end
end""")
    stra.flush("Animated ad screens")
    street_props(pix, aur, models, blds)
    city_life(stra, aur, models, blds, anchors, studio)
    wet_air(aur, stra, studio, models)

    # --- Aurora: night, rain, haze --------------------------------------------------------------
    aur.call("environment_update", skyMode="gradient", skyTop="#070612", skyHorizon="#36203f", ground="#1e1624",
             sunElevation=35, sunAzimuth=200, sunIntensity=0.04, sunColor="#8fa8ff", ambient=1.0, reflections=1.0,
             fogColor="#2a1c38", fogDensity=0.0045, fogHeight=0.02, haze=0.006, godRays=1.5,
             exposure=1.5, autoExposure=False, tonemap="agx", bloomIntensity=0.55, bloomThreshold=1.2,
             saturation=1.12, contrast=1.1, vignette=0.32, grain=0.06, chromaticAberration=0.15,
             look="teal_orange", lookStrength=0.45, gi=1, ssr=1, ao=1.0, windSpeed=2.0, windDirection=200, showGrid=False)
    aur.call("fx_create", effect="rain", name="Rain", position=[0, 22, -40],
             overrides={"shapeSize": [2 * FACADE + 2, 0, 170], "rate": 16000, "maxParticles": 40000, "floorHeight": 0.02,
                        "colorStart": "#c8d4ea8c", "colorEnd": "#c8d4ea8c", "sizeStart": 0.012, "sizeEnd": 0.012})
    nim.e("Player Camera", pos=(2.5, 1.7, 30), rot=(-2, 0, 0), camera={"fov": 55, "primary": True, "farPlane": 2500})
    nim.flush("Game camera")
    direct_shots(nim)
    nim.call("scene_save", path="scenes/main.sky.json")


def restyle(pix, path, src_mat_path, tint=1.0):
    """Re-points an imported material at a photoscanned set (triplanar), keeping it per-building tintable."""
    with open(os.path.join(pix.studio.project, src_mat_path)) as f:
        src = json.load(f)
    c = max(0, min(255, int(204 * tint)))
    pix.material(path, color=f"#{c:02x}{c:02x}{min(255, c + 4):02x}", texture=src["texture"], normalMap=src["normalMap"],
                 ormMap=src["ormMap"], metallic=1.0, roughness=1.0, tilingU=src["tilingU"], tilingV=src["tilingV"], triplanar=True,
                 occlusionStrength=1.0, emissive=[0, 0, 0, 1])


# =============================================================================================
# Hero shots: each is a camera path (Catmull-Rom through `points`, looking at `target`) played by
# its own sequence (cinematics/<name>.sequence.json); the still is taken at the middle point.
# =============================================================================================
SHOTS = [
    dict(name="avenue_dolly", points=[(2.4, 1.7, 31), (1.7, 1.75, 21), (1.0, 1.8, 11)], target=(-0.5, 13, -90), fov=52, dur=7,
         warm=150),
    dict(name="puddle_mirror", points=[(1.0, 0.34, 4.5), (0.5, 0.3, 1.8), (0.0, 0.28, -0.9)], target=(-1.2, 3.5, -45), fov=42,
         aperture=2.8, focus=6, dur=6, warm=150),
    dict(name="noodle_stall", points=[(-5.4, 1.5, -9.0), (-6.0, 1.45, -11.5), (-6.5, 1.4, -14.0)], target=(-8.6, 1.7, -24),
         fov=38, aperture=2.2, focus=9.5, dur=7, warm=150),
    dict(name="crane_reveal", points=[(1.5, 2.0, -50), (1.0, 18, -47), (0.5, 42, -44)], target=(0, 22, -190), fov=55, dur=8,
         warm=150),
    dict(name="hologram", points=[(4.0, 23, -27), (3.0, 24, -31), (2.0, 25, -35)], target=(0, 31, -77), fov=45, dur=7,
         warm=300),
    dict(name="monorail", points=[(-10, 9, -57), (-7, 11, -59), (-4, 13, -61)], target=(6, 22, -77), fov=55, dur=6,
         warm=560),
    dict(name="skyline", points=[(-4, 92, 80), (0, 95, 70), (4, 98, 60)], target=(0, 30, -250), fov=50, dur=8, warm=150),
    dict(name="blade_canyon", points=[(5.6, 1.6, -95), (5.4, 1.7, -99.5), (5.2, 1.8, -104)], target=(-9, 24, -130), fov=30,
         dur=7, warm=150),
]


def catmull(points, t):
    pts = [points[0]] + list(points) + [points[-1]]
    n = len(points) - 1
    seg = min(n - 1, int(t * n))
    u = t * n - seg
    p0, p1, p2, p3 = pts[seg], pts[seg + 1], pts[seg + 2], pts[seg + 3]
    return [0.5 * ((2 * p1[i]) + (-p0[i] + p2[i]) * u + (2 * p0[i] - 5 * p1[i] + 4 * p2[i] - p3[i]) * u * u +
                   (-p0[i] + 3 * p1[i] - 3 * p2[i] + p3[i]) * u ** 3) for i in range(3)]


def direct_shots(nim):
    for sh in SHOTS:
        path = f"cinematics/{sh['name']}.sequence.json"
        cam = f"Cam {sh['name']}"
        nim.call("sequence_create", path=path, duration=sh["dur"], entity=f"Seq {sh['name']}", play_on_start=False, overwrite=True)
        nim.call("sequence_camera_shot", sequence=path, camera=cam, shot="path", start=0, duration=sh["dur"],
                 points=[list(p) for p in sh["points"]], target=list(sh["target"]), fov=sh["fov"], ease="smooth")
        lens = {"fov": sh["fov"], "primary": False, "farPlane": 2500}
        if sh.get("aperture"):
            lens.update(aperture=sh["aperture"], focusDistance=sh["focus"])
        nim.call("entity_update", entity=cam, components={"camera": lens})


def shots():
    """Film footage: every hero shot as its camera move (smoothstep along the path)."""
    out = []
    for sh in SHOTS:
        def cam(i, n, sh=sh):
            t = i / max(1, n - 1)
            e = t * t * (3 - 2 * t)
            v = dict(eye=catmull(sh["points"], e), target=list(sh["target"]), fov=sh["fov"])
            if sh.get("aperture"):
                v.update(aperture=sh["aperture"], focus_distance=sh["focus"])
            return v
        out.append(dict(name=sh["name"], frames=int(sh["dur"] * 30), cam=cam, warmup=sh.get("warm", 150)))
    return out


# =============================================================================================
# Street furniture (Poly Haven CC0 props + kit pieces)
# =============================================================================================
def street_props(pix, aur, models, blds):
    r = rnd(31337)
    A = {k: ph.model(pix, k) for k in (
        "covered_car", "metal_trash_can", "trashbag", "cardboard_box_01", "concrete_road_barrier", "fire_hydrant",
        "water_manhole_cover", "utility_box_01", "utility_box_02", "CoffeeCart_01", "bar_chair_round_01",
        "WetFloorSign_01", "plastic_crate_01", "plastic_crate_03", "portable_generator", "propane_tank")}
    # parked (covered) cars along the curbs
    for k, (x, z, yaw) in enumerate([(5.2, 16, 0), (5.2, -24, 2), (-5.2, -108, 180), (5.2, -152, -2), (-5.2, 30, 178)]):
        ph.place(pix, A["covered_car"], f"Parked Car {k}", (x, 0.0, z), yaw=yaw)
    # the noodle stall on the left sidewalk, stools, crates, lanterns
    stall_z = -21.0
    ph.place(pix, models["prop_noodle_stall"], "Noodle Stall", (-FACADE + 1.75, 0.15, stall_z), yaw=90)
    for i in range(4):
        ph.place(pix, A["bar_chair_round_01"], f"Stall Stool {i}", (-FACADE + 3.0, 0.15, stall_z - 1.2 + i * 0.8), yaw=r.uniform(0, 360))
    for i in range(3):
        ph.place(pix, A["plastic_crate_01" if i % 2 else "plastic_crate_03"], f"Stall Crate {i}",
                 (-FACADE + 0.5, 0.15 + 0.32 * (i // 2), stall_z + 2.6 + 0.6 * (i % 2)), yaw=r.uniform(-15, 15))
    ph.place(pix, A["propane_tank"], "Stall Propane", (-FACADE + 0.6, 0.15, stall_z - 2.2))
    for i in range(5):
        ph.place(pix, models["prop_lantern"], f"Lantern {i}", (-FACADE + 2.55, 2.05, stall_z - 1.6 + i * 0.8))
    aur.light("Stall Warm Light", (-FACADE + 2.4, 2.1, stall_z), "#ffb070", 6, 7.5)
    aur.light("Lantern Glow", (-FACADE + 2.8, 1.8, stall_z), "#ff4a2a", 5, 6)
    ph.place(pix, A["CoffeeCart_01"], "Coffee Cart", (FACADE - 1.6, 0.15, -57), yaw=-90)
    aur.light("Coffee Cart Light", (FACADE - 2.4, 2.2, -57), "#ffd8a0", 5, 6)
    # dumpsters, bags and boxes by the walls
    spots = [(-1, 8), (1, -2), (-1, -48), (1, -64), (-1, -92), (1, -118), (-1, -160), (1, -188), (-1, -230)]
    for k, (side, z) in enumerate(spots):
        x = side * (FACADE - 1.0)
        ph.place(pix, A["metal_trash_can"], f"Dumpster {k}", (x, 0.15, z), yaw=90 if side < 0 else -90)
        for j in range(r.randint(2, 5)):
            ph.place(pix, A["trashbag"], f"Trash Bag {k}.{j}", (x - side * r.uniform(0.6, 1.3), 0.15, z + r.uniform(-2.0, 2.0)),
                     yaw=r.uniform(0, 360), scale=r.uniform(0.9, 1.25))
        if r.random() < 0.6:
            for j in range(r.randint(1, 3)):
                ph.place(pix, A["cardboard_box_01"], f"Box {k}.{j}", (x - side * r.uniform(0.3, 1.0), 0.15, z + 1.6 + j * 0.55),
                         yaw=r.uniform(0, 360), scale=r.uniform(1.2, 1.8))
    for k, (x, z) in enumerate([(6.1, 6), (-6.1, -96), (6.1, -205)]):
        ph.place(pix, A["fire_hydrant"], f"Hydrant {k}", (x, 0.15, z), yaw=90)
    for k, (side, z) in enumerate([(1, -12), (-1, -66), (1, -99), (-1, -140), (1, -170)]):
        ph.place(pix, A["utility_box_01" if k % 2 else "utility_box_02"], f"Utility Box {k}", (side * (FACADE - 0.45), 0.15, z),
                 yaw=90 if side < 0 else -90)
    for k, (x, z, yaw) in enumerate([(3.6, -196, 8), (1.0, -199, -6), (-1.6, -197, 3)]):
        ph.place(pix, A["concrete_road_barrier"], f"Road Barrier {k}", (x, 0.0, z), yaw=yaw + 90)
    ph.place(pix, A["WetFloorSign_01"], "Wet Floor Sign", (8.2, 0.15, -38), yaw=30)
    ph.place(pix, A["portable_generator"], "Generator", (-FACADE + 0.7, 0.15, stall_z + 3.6), yaw=90)
    for k, (x, z) in enumerate(MANHOLES):
        ph.place(pix, A["water_manhole_cover"], f"Manhole {k}", (x, 0.005, z), yaw=r.uniform(0, 360))
    for side in (-1, 1):
        for zz in (CROSS[1] + 0.8, CROSS[0] - 0.8):
            for i in range(4):
                ph.place(pix, models["prop_bollard"], f"Bollard {side} {int(zz)} {i}", (side * (ROAD + 0.6 + i * 1.0), 0.15, zz))
    pix.flush("Street furniture (Poly Haven CC0) and the noodle stall")


MANHOLES = [(-2.0, 12), (2.2, -14), (-1.4, -52), (1.8, -101), (-2.4, -146), (2.0, -176)]


# =============================================================================================
# Life: walkers, drones, traffic, monorail
# =============================================================================================
def city_life(stra, aur, models, blds, anchors, studio):
    r = rnd(1984)
    pix = studio.agent("Pixel")

    # --- walkers (CesiumMan, CC-BY 4.0) in plain coats, some with clear umbrellas ---------------
    if os.path.exists(os.path.join(studio.project, "downloads/cesium_man/CesiumMan.glb")):
        man = pix.call("asset_import", path="downloads/cesium_man/CesiumMan.glb")
    else:
        man = pix.call("asset_download",
                       url="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/CesiumMan/glTF-Binary/CesiumMan.glb",
                       folder="downloads/cesium_man", license="CC-BY-4.0", author="Cesium (Khronos glTF Sample Assets)",
                       source_page="https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/CesiumMan")
    coats = []
    for i, (c, rough) in enumerate([("#1b1d22", 0.55), ("#2a2622", 0.7), ("#3a1418", 0.5), ("#14202a", 0.45), ("#262626", 0.35),
                                    ("#4a4038", 0.75)]):
        path = f"materials/coat_{i}.mat.json"
        pix.material(path, color=c, roughness=rough, metallic=0.0, clearcoat=0.6 if rough < 0.5 else 0.0, texture="")
        coats.append(path)
    walkers = []
    lanes = [(-8.3, -1), (-7.4, 1), (7.4, -1), (8.3, 1), (-8.0, -1), (7.9, 1), (-7.6, 1), (8.1, -1), (-8.4, -1), (7.5, 1),
             (-7.9, 1), (8.4, -1)]
    for k, (x, d) in enumerate(lanes):
        name = f"Walker {k}"
        pix.op("prefab_instantiate", prefab=man["prefab"], name=name, position=[x, 0.15, r.uniform(-190, 36)],
               rotation=[0, 0 if d < 0 else 180, 0], scale=r.uniform(1.0, 1.1))
        walkers.append((name, d))
    pix.flush("Pedestrians")
    for k, (name, d) in enumerate(walkers):
        stra.call("entity_update", entity=name, vars={"dir": d, "speed": round(r.uniform(1.05, 1.35), 2)})
        set_coat(stra, name, coats[k % len(coats)])
        if k % 3 != 2:
            ph.place(stra, models["prop_umbrella_" + r.choice(["cyan", "pink", "amber"])], f"{name} Umbrella", (0.18, 1.12, -0.05),
                     parent=name)
    stra.flush("Umbrellas")
    for name, d in walkers:
        stra.behave(name, "Stroll", "Walk the sidewalk through the rain; wrap around at the ends of the block.", """
var dir = -1
var speed = 1.2
on tick
  move self by (0, 0, dir * speed * dt)
  if self.position.z < -200 then
    self.position.z = 40
  elif self.position.z > 40 then
    self.position.z = -200
  end
end""")
    stra.flush("Pedestrians stroll")

    # --- drones with blinking beacons -------------------------------------------------------------
    for k in range(9):
        cx, cy, cz = r.uniform(-5, 5), r.uniform(9, 26), r.uniform(-150, 10)
        name = f"Drone {k}"
        ph.place(stra, models["prop_drone"], name, (cx, cy, cz))
        stra.e(f"{name} Beacon", "sphere", "#000000", (0, -0.12, 0.2), scale=0.06, parent=name, emissive=[1, 0.1, 0.1, 12], unlit=True,
               castShadows=False)
        stra.light(f"{name} Lamp", (0, -0.3, 0), r.choice(["#9fe8ff", "#ffffff", "#ff7ad0"]), 1.6, 5, parent=name)
        stra.op("entity_update", entity=name, vars={"cx": cx, "cy": cy, "cz": cz, "ax": r.uniform(3, 7), "az": r.uniform(6, 18),
                                                     "w": r.uniform(0.15, 0.35), "ph": r.uniform(0, 6.28)})
    stra.flush("Drones")
    for k in range(9):
        stra.behave(f"Drone {k}", "Patrol", "Drift on a slow figure-eight over the avenue, bobbing in the wind.", """
var cx = 0
var cy = 15
var cz = 0
var ax = 4
var az = 10
var w = 0.2
var ph = 0
on tick
  let a = time * w + ph
  self.position = (cx + sin(a) * ax, cy + sin(a * 2.3) * 0.6, cz + sin(a * 0.5) * az)
  self.rotation = (sin(a * 1.7) * 6, a * 30, cos(a * 1.3) * 6)
end""")
        stra.behave(f"Drone {k} Beacon", "Blink", "Strobe twice a second.", """
on tick
  if (time * 2 + self.parent.position.x) % 1 < 0.12 then
    self.mesh.emissive = color(1, 0.1, 0.1, 14)
  else
    self.mesh.emissive = color(1, 0.1, 0.1, 0.2)
  end
end""")
    stra.flush("Drone patrols")

    # --- flying traffic in sky lanes ------------------------------------------------------------
    for k in range(16):
        lane = k % 4
        y = [38.0, 58.0, 76.0, 96.0][lane]
        x = (-3.2 if k % 2 else 3.2) + r.uniform(-1, 1)
        d = -1 if k % 2 else 1
        name = f"Skycar {k}"
        ph.place(stra, models["prop_hover_car_" + ("a" if k % 3 else "b")], name, (x, y, r.uniform(-380, 60)), yaw=0 if d < 0 else 180)
        stra.op("entity_update", entity=name, vars={"dir": d, "speed": r.uniform(14, 26) + lane * 2})
        if k % 4 == 0:
            stra.light(f"{name} Headlight", (0, 0.2, -3.0), "#e8f0ff", 6, 22, kind="spot", rot=(-12, 0, 0), spot=28, parent=name)
    stra.flush("Sky traffic")
    for k in range(16):
        stra.behave(f"Skycar {k}", "Lane", "Fly the sky lane along the avenue; loop back far away.", """
var dir = -1
var speed = 20
on tick
  move self by (0, sin(time * 0.7 + speed) * 0.02, dir * speed * dt)
  if self.position.z < -420 then
    self.position.z = 80
  elif self.position.z > 80 then
    self.position.z = -420
  end
end""")
    stra.flush("Sky traffic flies")

    # --- monorail: three cars gliding over Ninth Street ---------------------------------------------
    stra.e("Monorail Train", pos=(160, RAIL_H + 0.15, RAIL_Z), tags=["train"], vars={"speed": 17.0})
    for i, part in enumerate(("rail_car_head", "rail_car_mid", "rail_car_tail")):
        ph.place(stra, models[part], f"Monorail Car {i}", (i * 15.6, 0, 0), parent="Monorail Train")
        stra.light(f"Monorail Car {i} Glow", (i * 15.6, 2.0, 0), "#ffd8b0", 5, 9, parent="Monorail Train")
    stra.light("Monorail Headlight", (-9.5, 1.2, 0), "#f0f4ff", 30, 40, kind="spot", rot=(-6, 90, 0), spot=22, parent="Monorail Train")
    stra.flush("Monorail train")
    stra.behave("Monorail Train", "Run", "Glide across Ninth Street every half minute.", """
var speed = 17
on tick
  move self by (0 - speed * dt, 0, 0)
  if self.position.x < -260 then
    self.position.x = 260
  end
end""")
    stra.flush("Monorail runs")


def set_coat(b, name, coat):
    """Gives one walker its own coat (the prefab's mesh may sit on the root or on a child part)."""
    ent = b.call("entity_get", entity=name)
    if not isinstance(ent, dict):
        return
    if "mesh" in ent.get("components", {}):
        b.call("entity_update", entity=name, components={"mesh": {"material": coat}})
    for child in ent.get("children", []):
        if isinstance(child, dict) and "id" in child:
            b.call("entity_update", entity=child["id"], components={"mesh": {"material": coat}})


# =============================================================================================
# Steam, puddles and the holographic giant over Ninth Street
# =============================================================================================
def wet_air(aur, stra, studio, models):
    r = rnd(555)
    for k in (0, 1, 3):
        x, z = MANHOLES[k]
        aur.call("fx_create", effect="steam_vent", name=f"Manhole Steam {k}", position=[x, 0.02, z])
    aur.call("fx_create", effect="steam", name="Stall Steam", position=[-FACADE + 1.75 + 0.45, 1.5, -21.4],
             overrides={"rate": 10, "sizeStart": 0.15, "sizeEnd": 0.9, "colorStart": "#e8e4e060", "colorEnd": "#e8e4e000"})
    aur.call("fx_create", effect="mist", name="Street Mist", position=[0, 0.4, -60],
             overrides={"shapeSize": [2 * FACADE, 0.6, 200], "rate": 6, "colorStart": "#8a8aa82a"})
    # puddles: shallow water bodies that mirror the neon (road and sidewalks)
    spots = [(-1.5, 22, 3.6), (2.6, 8, 2.8), (-0.6, -6, 4.4), (3.4, -18, 3.0), (-3.2, -30, 3.8), (1.0, -44, 5.0), (-2.4, -58, 3.2),
             (2.0, -92, 4.0), (-1.2, -112, 3.6), (3.0, -126, 2.8), (-3.0, -140, 4.2), (0.8, -160, 3.4), (-7.8, -4, 2.2),
             (8.0, -30, 2.0), (-8.2, -50, 2.4), (7.6, -96, 2.2), (0.2, 34, 3.0), (-4.4, 14, 2.4)]
    for k, (x, z, size) in enumerate(spots):
        y = 0.008 if abs(x) < ROAD else 0.158
        aur.call("fx_create", effect="puddle", name=f"Puddle {k}", position=[x, y, z], overrides={"size": size * r.uniform(0.9, 1.2)})

    # the hologram: a giant figure walking on a turntable of light above the crossing
    man = "downloads/cesium_man/CesiumMan.prefab.json"
    holo_y = RAIL_H + 4.2
    aur.op("prefab_instantiate", prefab=man, name="Hologram", position=[0, holo_y, RAIL_Z], rotation=[0, 30, 0], scale=9.0)
    aur.flush("Hologram figure")
    ent = aur.call("entity_get", entity="Hologram")
    targets = ["Hologram"] if "mesh" in ent.get("components", {}) else [c["id"] for c in ent.get("children", []) if isinstance(c, dict)]
    for t in targets:
        aur.call("entity_update", entity=t, components={"mesh": {"material": "", "texture": "", "color": "#5ae6ff22",
                                                                  "emissive": [0.3, 0.85, 1.0, 1.6], "rim": 2.0, "roughness": 0.3,
                                                                  "castShadows": False, "doubleSided": True}})
    for i, rad in enumerate((3.6, 2.4, 4.4)):
        ph.place(aur, models[f"prop_holo_ring_{i}"], f"Holo Ring {i}", (0, holo_y - 0.05 + i * 0.12, RAIL_Z))
    aur.e("Holo Beam", "cone", [0.3, 0.85, 1.0, 0.06], (0, holo_y + 8, RAIL_Z), rot=(180, 0, 0), scale=(10, 16, 10),
          emissive=[0.2, 0.75, 1.0, 0.5], unlit=True, castShadows=False)
    aur.light("Holo Light", (0, holo_y + 6, RAIL_Z + 3), "#40d8ff", 26, 40)
    aur.light("Holo Under Light", (0, holo_y - 1.5, RAIL_Z), "#40d8ff", 12, 18)
    aur.flush("Hologram projector")
    stra.behave("Hologram", "Turntable", "The giant turns slowly on its pad and glitches now and then.", """
var glitch = 0
on tick
  if chance(0.004) then
    glitch = random(0.08, 0.3)
  end
  glitch = max(0, glitch - dt)
  let s = 9
  if glitch > 0 then
    s = 9 * (1 + 0.06 * sin(time * 90))
    self.position.x = sin(time * 140) * 0.25
  else
    self.position.x = 0
  end
  self.scale = (s, 9, s)
  self.rotation = (0, 30 + time * 9, 0)
end""")
    stra.behave("Holo Beam", "Shimmer", "The projector beam breathes.", """
on tick
  self.mesh.emissive = color(0.2, 0.75, 1.0, 0.45 + 0.15 * sin(time * 3.1) + 0.1 * sin(time * 17))
end""")
    for i in range(3):
        stra.behave(f"Holo Ring {i}", "Spin", "The projector rings turn, each at its own pace.", f"""
on tick
  self.rotation = (0, time * {(i + 1) * 23 * (-1) ** i}, 0)
end""")
    stra.flush("Hologram turns")


def render_stills(project, out_dir, width=1920, height=1080, samples=24, only=None):
    """Final stills of every hero shot (middle of its move) as JPEGs (quality 90)."""
    import subprocess

    from sky import Sky
    sky = Sky(project=project, scene="scenes/main.sky.json")
    sky.call("camera_set", eye=[0, 0, 40], target=[0, 0, 0])  # custom views inherit a 2 km far plane from the editor camera
    os.makedirs(os.path.join(project, out_dir), exist_ok=True)
    sky.call("sim_control", action="play")
    done = 0
    for sh in sorted(SHOTS, key=lambda s: s.get("warm", 150)):
        if only and sh["name"] not in only:
            continue
        need = sh.get("warm", 150) - done
        if need > 0:
            sky.call("sim_control", action="step", ticks=need)
            done += need
        args = dict(width=width, height=height, annotate=False, overlays=False, include_image=False, samples=samples,
                    eye=catmull(sh["points"], 0.5), target=list(sh["target"]), fov=sh["fov"],
                    save_path=f"{out_dir}/{sh['name']}.png")
        if sh.get("aperture"):
            args.update(aperture=sh["aperture"], focus_distance=sh["focus"])
        sky.call("viewport_capture", **args)
        png = os.path.join(project, out_dir, sh["name"] + ".png")
        jpg = png[:-4] + ".jpg"
        subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "90", png, "--out", jpg], check=True,
                       capture_output=True)
        os.remove(png)
        print(sh["name"], os.path.getsize(jpg) // 1024, "KB", flush=True)
    sky.close()


if __name__ == "__main__":
    import sys
    sys.path.insert(0, os.path.dirname(HERE))
    root = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
    if len(sys.argv) > 1 and sys.argv[1] == "stills":
        render_stills(os.path.join(root, "examples", "neon_requiem"), "shots", only=sys.argv[2:] or None)
