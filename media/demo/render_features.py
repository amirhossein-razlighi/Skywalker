#!/usr/bin/env python3
"""Feature footage for the demo video: material look-dev, toon vs PBR, time of day, texture
swatches, four-view capture. Everything is rendered by the engine through its tools.

  render_features.py WORK_DIR
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from sky import ROOT, Sky  # noqa: E402

W, H = 1920, 1080
out = os.path.join(sys.argv[1], "features")
os.makedirs(out, exist_ok=True)
lookdev = os.path.join(sys.argv[1], "lookdev_project")
import shutil  # noqa: E402
shutil.rmtree(lookdev, ignore_errors=True)
os.makedirs(lookdev, exist_ok=True)


def cap(sky, path, **kw):
    sky.call("viewport_capture", width=W, height=H, annotate=False, overlays=False, include_image=False, save_path=path, **kw)


def clip(sky, name, n, cam, before=None, ticks=2):
    d = os.path.join(out, name)
    os.makedirs(d, exist_ok=True)
    for i in range(n):
        if before:
            before(i, n)
        cap(sky, os.path.join(d, f"{i:04d}.png"), **cam(i, n))
        sky.call("sim_control", action="step", ticks=ticks)
    print(name, n, flush=True)


# --- Material look-dev: presets + procedural textures under an atmospheric sky ---------------
s = Sky(project=lookdev)
s.call("scene_new", name="Material Lookdev", empty=True)
mats = {}
for kind, colors in [("bricks", {}), ("planks", {}), ("marble", {}), ("rust", {}), ("cobblestone", {}), ("scales", {"color1": "#2a7a5a", "color2": "#4ab88a"})]:
    mats[kind] = s.call("texture_generate", kind=kind, name=f"textures/{kind}", tiling=0.8, **colors)["material"]
for p in ["gold", "chrome", "car_paint", "glass", "skin", "ceramic", "toon", "velvet"]:
    s.call("material_create", path=f"materials/{p}.mat.json", preset=p)
ops = [("entity_create", {"name": "Floor", "mesh": "plane", "scale": [80, 1, 80], "components": {"mesh": {"material": mats["planks"]}}})]
row1 = ["gold", "chrome", "car_paint", "glass", "skin", "ceramic", "toon", "velvet"]
for k, p in enumerate(row1):
    ops.append(("entity_create", {"name": p, "mesh": "sphere", "position": [-7 + k * 2, 0.9, 0], "scale": [1.7] * 3,
                                  "components": {"mesh": {"material": f"materials/{p}.mat.json"}}}))
for k, kind in enumerate(mats):
    if kind == "planks":
        continue
    ops.append(("entity_create", {"name": kind, "mesh": "cube" if k % 2 else "cylinder", "position": [-6 + k * 2.6, 1.0, -3.2],
                                  "scale": [1.8, 2.0, 1.8], "components": {"mesh": {"material": mats[kind]}}}))
ops.append(("entity_create", {"name": "Back Wall", "mesh": "cube", "position": [0, 2, -6], "scale": [18, 4, 0.4],
                              "components": {"mesh": {"material": mats["bricks"]}}}))
s.call("batch", label="lookdev", operations=[{"tool": t, "args": a} for t, a in ops])
s.call("environment_update", skyMode="atmosphere", clouds=0.45, sunElevation=22, sunAzimuth=140, sunIntensity=2.4,
       sunColor="#ffe6c8", ambient=0.5, fogDensity=0.003, fogColor="#c8d4e8", showGrid=False, tonemap="agx", ao=1.0)
s.call("scene_save", path="scenes/main.sky.json")
s.call("sim_control", action="play")


def orbit(i, n):
    t = i / (n - 1)
    a = math.radians(-35 + 70 * t)
    return dict(eye=[math.sin(a) * 9.5, 2.4 - 0.6 * t, math.cos(a) * 9.5 + 1], target=[0, 1.0, -1])


clip(s, "lookdev", 180, orbit)
# Texture swatches (front-on, one per kind)
sw = os.path.join(out, "swatches")
os.makedirs(sw, exist_ok=True)
kinds = ["bricks", "planks", "cobblestone", "marble", "wood", "rust", "metal_brushed", "tiles", "rock", "sand", "grass",
         "fabric", "scales", "hexagons", "stylized", "dirt"]
for k in kinds:
    r = s.call("texture_generate", kind=k, name=f"swatch/{k}", size=512, create_material=False)
    os.replace(os.path.join(lookdev, r["albedo"]), os.path.join(sw, f"{k}.png"))
s.close()

# --- Toon vs PBR (Cloudhopper) ------------------------------------------------------------------
c = Sky(project=os.path.join(ROOT, "examples", "cloudhopper"), scene="scenes/main.sky.json")
c.call("sim_control", action="play")


def aerial(i, n):
    t = i / (n - 1)
    a = math.radians(60 + 30 * t)
    return dict(eye=[math.cos(a) * 30, 15, math.sin(a) * 30], target=[0, 1, 0])


clip(c, "toon", 120, aerial)
c.call("sim_control", action="stop")
ov = c.call("scene_overview", max_entities=5000)
ops = [{"tool": "entity_update", "args": {"entity": e["id"], "components": {"mesh": {"shading": "pbr", "outline": 0, "rim": 0}}}}
       for e in ov["entities"] if "mesh" in e.get("components", [])]
c.call("batch", label="PBR look", operations=ops)
c.call("environment_update", tonemap="agx")
c.call("sim_control", action="play")
clip(c, "pbr", 120, aerial)
c.close()

# --- Time of day (Harvest Fair, atmospheric sky) -------------------------------------------------
h = Sky(project=os.path.join(ROOT, "examples", "harvest_fair"), scene="scenes/main.sky.json")
h.call("sim_control", action="play")
h.call("sim_control", action="step", ticks=60)


def tod(i, n):
    t = i / (n - 1)
    elev = 38 - 50 * t  # afternoon -> sunset -> night
    patch = {"sunElevation": elev, "stars": max(0.0, min(1.0, (2 - elev) / 8)), "ambient": max(0.12, 0.5 * min(1, (elev + 6) / 20))}
    h.call("environment_update", **patch)


clip(h, "timeofday", 210, lambda i, n: dict(eye=[24, 10, 26], target=[-2, 5, -8]), before=tod)
h.close()

# --- Four-view capture (Hollow Manor) ------------------------------------------------------------
m = Sky(project=os.path.join(ROOT, "examples", "hollow_manor"), scene="scenes/main.sky.json")
r = m._rpc("tools/call", {"name": "viewport_multi", "arguments": {"size": 540}})
import base64  # noqa: E402
for block in r["content"]:
    if block["type"] == "image":
        open(os.path.join(out, "multiview.png"), "wb").write(base64.b64decode(block["data"]))
m.close()
print("FEATURES DONE")
