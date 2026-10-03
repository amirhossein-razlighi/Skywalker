#!/usr/bin/env python3
"""Look-dev renders for hair & fur and GPU particles (docs/HAIR_AND_VFX.md).

Everything is built and rendered by the engine through its MCP tools, like an agent would:

  hair_vfx.py [OUT_DIR] [--only hair,fur,vfx,bench]

Writes PNG stills to OUT_DIR (default: a temp folder) and, when `magick` is installed, small
JPEGs into docs/images/. `bench` prints GPU timings for 1M particles and a 100k-strand groom.
"""
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from sky import ROOT, Sky  # noqa: E402

if not os.environ.get("SKY_CLI"):
    for cand in ("build/headless/bin/skywalker", "build/bin/skywalker"):
        if os.path.exists(os.path.join(ROOT, cand)):
            os.environ["SKY_CLI"] = os.path.join(ROOT, cand)
            break
    import sky as _sky  # noqa: E402
    _sky.CLI = os.environ.get("SKY_CLI", _sky.CLI)

args = [a for a in sys.argv[1:] if not a.startswith("--")]
OUT = args[0] if args else os.path.join(tempfile.gettempdir(), "skywalker_hair_vfx")
ONLY = set()
for a in sys.argv[1:]:
    if a.startswith("--only="):
        ONLY = set(a.split("=", 1)[1].split(","))
os.makedirs(OUT, exist_ok=True)
DOCS = os.path.join(ROOT, "docs", "images")


def wanted(name):
    return not ONLY or name in ONLY


def publish(png, name, width=1280):
    """Small JPEG for the docs (needs ImageMagick)."""
    if shutil.which("magick"):
        os.makedirs(DOCS, exist_ok=True)
        subprocess.run(["magick", png, "-resize", f"{width}x", "-quality", "84", os.path.join(DOCS, name)], check=True)


def smooth(e0, e1, x):
    t = min(max((x - e0) / (e1 - e0), 0.0), 1.0)
    return t * t * (3 - 2 * t)


def write_head_obj(path, nu=144, nv=96, scalp=False):
    """A sculpted mannequin head (meters, face toward +Z). scalp=True paints the hair region into
    vertex color R (vertex colors tint the surface, so the scalp is a separate, invisible copy)."""
    a, b, c = 0.077, 0.108, 0.098
    verts, cols, faces = [], [], []
    hair_line = [(0, 0.60), (40, 0.52), (70, 0.36), (95, 0.18), (115, -0.12), (150, -0.40), (180, -0.50)]

    def hairline(phi_deg):
        p = abs(phi_deg)
        for (p0, h0), (p1, h1) in zip(hair_line, hair_line[1:]):
            if p <= p1:
                return h0 + (h1 - h0) * (p - p0) / (p1 - p0)
        return hair_line[-1][1]

    for j in range(nv + 1):
        th = math.pi * j / nv
        for i in range(nu):
            ph = 2 * math.pi * i / nu
            d = (math.sin(th) * math.sin(ph), math.cos(th), math.sin(th) * math.cos(ph))
            cz = c * (1.06 if d[2] < 0 else 1.0)
            r = 1.0 / math.sqrt((d[0] / a) ** 2 + (d[1] / b) ** 2 + (d[2] / cz) ** 2)
            x, y, z = d[0] * r, d[1] * r, d[2] * r
            front = max(d[2], 0.0)
            # Jaw and chin: narrower below the cheekbones, a little forward at the chin.
            x *= 1 - 0.30 * smooth(-0.015, -0.1, y)
            z -= 0.02 * smooth(-0.03, -0.1, y) * (1 - front)
            bump = 0.0
            bump += 0.021 * math.exp(-((x / 0.011) ** 2 + ((y + 0.012) / 0.026) ** 2)) * front ** 2  # nose
            bump += 0.006 * math.exp(-((y - 0.032) / 0.011) ** 2) * front ** 4  # brow ridge
            bump -= 0.009 * math.exp(-(((abs(x) - 0.031) / 0.013) ** 2 + ((y - 0.012) / 0.011) ** 2)) * front  # eye sockets
            bump += 0.005 * math.exp(-(((abs(x) - 0.042) / 0.016) ** 2 + ((y + 0.012) / 0.014) ** 2)) * front  # cheekbones
            bump += 0.005 * math.exp(-((x / 0.02) ** 2 + ((y + 0.048) / 0.008) ** 2)) * front ** 3  # lips
            bump += 0.009 * math.exp(-((x / 0.022) ** 2 + ((y + 0.088) / 0.016) ** 2)) * front ** 2  # chin
            bump += 0.013 * math.exp(-((y / 0.026) ** 2 + ((z + 0.006) / 0.013) ** 2)) * abs(d[0]) ** 6  # ears
            px, py, pz = x + d[0] * bump, y + d[1] * bump, z + d[2] * bump
            verts.append((px, py, pz))
            phi = math.degrees(math.atan2(d[0], d[2]))
            h = hairline(phi)
            mask = smooth(h - 0.04, h + 0.06, d[1])
            if abs(d[0]) > 0.9 and abs(d[1]) < 0.3:  # keep the ears clear
                mask *= 0.0
            cols.append((mask, 0.0, 0.0))
    for j in range(nv):
        for i in range(nu):
            i2 = (i + 1) % nu
            a0, b0 = j * nu + i, j * nu + i2
            a1, b1 = (j + 1) * nu + i, (j + 1) * nu + i2
            if j > 0:
                faces.append((a0, a1, b0))
            if j < nv - 1:
                faces.append((b0, a1, b1))
    with open(path, "w") as f:
        f.write("# Skywalker look-dev mannequin head (meters); vertex color R = scalp\n")
        for (x, y, z), (r, g, bb) in zip(verts, cols):
            f.write(f"v {x:.5f} {y:.5f} {z:.5f} {r:.3f} {g:.3f} {bb:.3f}\n" if scalp else f"v {x:.5f} {y:.5f} {z:.5f}\n")
        for t in faces:
            f.write(f"f {t[0] + 1} {t[1] + 1} {t[2] + 1}\n")


def cap(s, name, eye, target, w=1600, h=900, samples=24, fov=30, **kw):
    path = os.path.join(OUT, name + ".png")
    s.call("viewport_capture", width=w, height=h, eye=eye, target=target, fov=fov, annotate=False, overlays=False,
           include_image=False, save_path=os.path.relpath(path, s.project), samples=samples, **kw)
    return path


class Session(Sky):
    def __init__(self, name):
        self.project = os.path.join(OUT, name + "_project")
        shutil.rmtree(self.project, ignore_errors=True)
        os.makedirs(self.project, exist_ok=True)
        super().__init__(project=self.project)

    def run(self, ticks, every=2, view=None):
        """Advances time with cheap frames so GPU simulations (hair, particles) keep up."""
        for _ in range(max(1, ticks // every)):
            self.call("sim_control", action="step", ticks=every)
            if view:
                self.call("viewport_capture", width=320, height=180, annotate=False, overlays=False, include_image=False,
                          samples=1, eye=view[0], target=view[1])


def studio(s, key_az=62, key_el=34, key=2.6):
    s.call("environment_update", skyMode="gradient", skyTop="#26282c", skyHorizon="#46464a", ground="#2c2a28", ambient=0.5,
           fogDensity=0, sunElevation=key_el, sunAzimuth=key_az, sunIntensity=key, sunColor="#fff2e4", ao=1,
           tonemap="agx", showGrid=False, bloomIntensity=0.2, vignette=0.3, exposure=1.0, shadowSoftness=1.5)


def bust(s, groom=None, overrides=None, hair_color=None):
    head = os.path.join(s.project, "meshes")
    os.makedirs(head, exist_ok=True)
    write_head_obj(os.path.join(head, "mannequin_head.obj"))
    write_head_obj(os.path.join(head, "mannequin_scalp.obj"), scalp=True)
    s.call("asset_import", path="meshes/mannequin_head.obj", normalize=False, create_entity="Head", position=[0, 1.62, 0])
    s.call("asset_import", path="meshes/mannequin_scalp.obj", normalize=False, create_entity="Scalp")
    s.call("entity_update", entity="Scalp", parent="Head", components={"transform": {"position": [0, 0, 0]},
                                                                       "mesh": {"visible": False, "castShadows": False}})
    skin = {"color": "#b98670", "roughness": 0.5, "subsurface": 0.7}
    s.call("entity_update", entity="Head", components={"mesh": skin})
    s.call("batch", label="bust", operations=[
        {"tool": "entity_create", "args": {"name": "Neck", "mesh": "cylinder", "position": [0, 1.49, -0.014],
                                           "scale": [0.108, 0.14, 0.104], "components": {"mesh": skin}}},
        {"tool": "entity_create", "args": {"name": "Torso", "mesh": "capsule", "position": [0, 1.34, -0.01], "rotation": [0, 0, 90],
                                           "scale": [0.5, 0.46, 0.24], "color": "#20262e",
                                           "components": {"mesh": {"roughness": 0.85}}}},
        {"tool": "entity_create", "args": {"name": "Floor", "mesh": "plane", "scale": [30, 1, 30], "color": "#3a3836",
                                           "components": {"mesh": {"roughness": 0.9}}}},
    ])
    if groom:
        ov = {"target": "Scalp", "maskChannel": "r", "maskAngle": 180, "colliders": "Torso, Neck"}
        ov.update(overrides or {})
        info = s.call("groom_create", entity="Head", preset=groom, overrides=ov)
        print("  groom:", {k: info.get(k) for k in ("strands", "guides", "generateMs", "gpuMemoryMB", "error")}, flush=True)


def lights(s, rim="#e8eeff", warm="#ffb070"):
    s.call("batch", label="lights", operations=[
        {"tool": "entity_create", "args": {"name": "Rim Light", "position": [-0.45, 1.95, -0.7],
                                           "components": {"light": {"kind": "point", "color": rim, "intensity": 4, "range": 4}}}},
        {"tool": "entity_create", "args": {"name": "Kicker", "position": [0.8, 1.5, -0.6],
                                           "components": {"light": {"kind": "point", "color": warm, "intensity": 1.2, "range": 4}}}},
    ])


# --- Hair ----------------------------------------------------------------------------------------
if wanted("hair"):
    looks = [
        ("hair_wavy", {"melanin": 0.7, "redness": 0.15}, "hair_wavy_brunette"),
        ("hair_straight", {"melanin": 0.18, "redness": 0.2, "length": 0.4}, "hair_straight_blond"),
        ("hair_curly", {"melanin": 0.88, "redness": 0.15}, "hair_curly_black"),
        ("hair_wavy", {"melanin": 0.5, "redness": 0.92, "wave": 0.02, "length": 0.32}, "hair_wavy_ginger"),
        ("hair_ponytail", {"melanin": 0.4, "redness": 0.3}, "hair_ponytail"),
    ]
    for preset, ov, name in looks:
        print(name, flush=True)
        s = Session(name)
        s.call("scene_new", name=name, empty=True)
        bust(s, preset, ov)
        studio(s)
        lights(s)
        s.run(30)
        three_q = ([0.5, 1.66, 0.62], [0, 1.55, -0.02])
        back = ([-0.38, 1.72, -0.55], [0, 1.55, -0.04])
        p = cap(s, name, *three_q)
        publish(p, name + ".jpg")
        if name in ("hair_wavy_brunette", "hair_curly_black"):
            p = cap(s, name + "_back", *back)
            publish(p, name + "_back.jpg", 960)
        if name == "hair_wavy_brunette":
            # Wind: the guides are simulated, children follow.
            s.call("environment_update", windSpeed=7, windDirection=250)
            s.run(40, view=three_q)
            p = cap(s, name + "_wind", *three_q, samples=8)
            publish(p, name + "_wind.jpg", 960)
            print("  info:", json.dumps(s.call("groom_info", entity="Head"))[:300], flush=True)
        s.close()

# --- Fur --------------------------------------------------------------------------------------------
if wanted("fur"):
    print("fur_creature", flush=True)
    s = Session("fur")
    s.call("scene_new", name="Fur", empty=True)
    orange, dark, cream = "#9c4a22", "#2e2420", "#d8c8b4"
    ops = [
        ("Floor", "plane", [0, 0, 0], [0, 0, 0], [30, 1, 30], "#4a463e", 0.9),
        ("Body", "capsule", [0, 0.44, 0], [0, 0, 90], [0.36, 0.64, 0.34], orange, 0.8),
        ("Head", "sphere", [0.37, 0.62, 0], [0, 0, 0], [0.25, 0.23, 0.23], orange, 0.8),
        ("Snout", "sphere", [0.49, 0.575, 0], [0, 0, -8], [0.17, 0.095, 0.11], cream, 0.8),
        ("Nose", "sphere", [0.575, 0.585, 0], [0, 0, 0], [0.034, 0.03, 0.036], "#141010", 0.15),
        ("Ear L", "cone", [0.34, 0.78, -0.07], [-14, 0, 8], [0.085, 0.13, 0.05], dark, 0.8),
        ("Ear R", "cone", [0.34, 0.78, 0.07], [14, 0, 8], [0.085, 0.13, 0.05], dark, 0.8),
        ("Tail", "capsule", [-0.46, 0.42, 0], [0, 0, 64], [0.14, 0.55, 0.14], orange, 0.8),
        ("Leg FL", "cylinder", [0.2, 0.17, -0.08], [0, 0, 0], [0.07, 0.36, 0.07], dark, 0.8),
        ("Leg FR", "cylinder", [0.2, 0.17, 0.08], [0, 0, 0], [0.07, 0.36, 0.07], dark, 0.8),
        ("Leg BL", "cylinder", [-0.19, 0.17, -0.08], [0, 0, 0], [0.07, 0.36, 0.07], dark, 0.8),
        ("Leg BR", "cylinder", [-0.19, 0.17, 0.08], [0, 0, 0], [0.07, 0.36, 0.07], dark, 0.8),
        ("Eye L", "sphere", [0.475, 0.655, -0.066], [0, 0, 0], [0.03] * 3, "#120c08", 0.1),
        ("Eye R", "sphere", [0.475, 0.655, 0.066], [0, 0, 0], [0.03] * 3, "#120c08", 0.1),
    ]
    s.call("batch", label="creature", operations=[
        {"tool": "entity_create", "args": {"name": n, "mesh": m, "position": p, "rotation": r, "scale": sc,
                                           "components": {"mesh": {"color": c, "roughness": ro}}}}
        for n, m, p, r, sc, c, ro in ops])
    fox = {"melanin": 0.62, "redness": 0.9, "colorVariation": 0.3, "tipColor": "#f4e2cc", "widthRoot": 0.05, "density": 3.5}
    s.call("groom_create", entity="Body", preset="fur_short",
           overrides=dict(fox, strands=300000, length=0.03, direction=[0, 1, 0], directionBlend=0.7, gravity=0.25))
    s.call("groom_create", entity="Head", preset="fur_short",
           overrides=dict(fox, strands=90000, length=0.016, direction=[-1, 0, 0], directionBlend=0.6))
    s.call("groom_create", entity="Snout", preset="fur_short",
           overrides=dict(fox, strands=30000, length=0.008, melanin=0.1, redness=0.3, tipColor="#ffffff", direction=[1, 0, 0]))
    s.call("groom_create", entity="Tail", preset="fur_long",
           overrides=dict(fox, strands=120000, length=0.1, direction=[0, 1, 0], directionBlend=0.6, tipColor="#fffaf2"))
    for ear in ("Ear L", "Ear R"):
        s.call("groom_create", entity=ear, preset="fur_short",
               overrides=dict(fox, strands=12000, length=0.01, melanin=0.92, redness=0.3, direction=[0, 1, 0]))
    for leg in ("Leg FL", "Leg FR", "Leg BL", "Leg BR"):
        s.call("groom_create", entity=leg, preset="fur_short",
               overrides=dict(fox, strands=16000, length=0.012, melanin=0.93, redness=0.3, direction=[0, -1, 0]))
    s.call("environment_update", skyMode="atmosphere", sunElevation=30, sunAzimuth=25, sunIntensity=2.6, clouds=0.3,
           ambient=0.5, fogDensity=0.0005, tonemap="agx", showGrid=False, ao=1, exposure=1.0, shadowSoftness=1.2)
    s.run(10)
    p = cap(s, "fur_creature", [1.15, 0.7, 1.25], [0.05, 0.47, 0], fov=34)
    publish(p, "fur_creature.jpg")
    p = cap(s, "fur_closeup", [0.85, 0.66, 0.42], [0.36, 0.6, 0.0], fov=30, samples=16)
    publish(p, "fur_closeup.jpg", 960)
    print("  info:", json.dumps(s.call("groom_info"))[:400], flush=True)
    s.close()

# --- GPU particles ------------------------------------------------------------------------------------
NIGHT = dict(skyMode="gradient", skyTop="#04060b", skyHorizon="#121826", ground="#0c0c10", ambient=0.12, sunIntensity=0.04,
             sunElevation=12, fogDensity=0.0, tonemap="agx", showGrid=False, bloomIntensity=0.35, exposure=1.1)


def stage(s, night=True):
    s.call("batch", label="stage", operations=[
        {"tool": "entity_create", "args": {"name": "Floor", "mesh": "plane", "scale": [80, 1, 80], "color": "#3a3a3e",
                                           "components": {"mesh": {"roughness": 0.3 if night else 0.8}}}},
        {"tool": "entity_create", "args": {"name": "Anvil", "mesh": "cube", "position": [0.9, 0.35, 0.3], "scale": [0.9, 0.7, 0.6],
                                           "color": "#3a3c40", "components": {"mesh": {"metallic": 1, "roughness": 0.35}}}},
        {"tool": "entity_create", "args": {"name": "Ball", "mesh": "sphere", "position": [-1.3, 0.5, 0.9], "scale": [1, 1, 1],
                                           "color": "#9a9aa2", "components": {"mesh": {"metallic": 1, "roughness": 0.18}}}},
        {"tool": "entity_create", "args": {"name": "Wall", "mesh": "cube", "position": [0, 2.5, -3.2], "scale": [14, 5, 0.4],
                                           "color": "#4a4440", "components": {"mesh": {"roughness": 0.85}}}},
    ])


if wanted("vfx"):
    shots = [
        # (name, effect, position, overrides, night, eye, target, warm seconds)
        ("vfx_sparks_shower", "sparks_shower", [0, 0, 0], {}, True, [3.6, 1.7, 4.2], [0.4, 0.8, 0], 3),
        ("vfx_fireworks", "fireworks", [0, 0, -12], {}, True, [0, 3, 20], [0, 10, -12], 6),
        ("vfx_magic_vortex", "magic_vortex", [0, 0, 0], {}, True, [3.2, 1.9, 4.2], [0, 1.4, 0], 3),
        ("vfx_ember_storm", "ember_storm", [0, 1, 0], {}, True, [5, 2, 7], [0, 2, 0], 4),
        ("vfx_falling_leaves", "falling_leaves", [0, 4.5, 1.5], {}, False, [2.2, 1.3, 3.6], [0, 1.7, 0], 12),
        ("vfx_snow_heavy", "snow_heavy", [0, 12, 0], {}, False, [6, 2.2, 8], [0, 1.2, 0], 6),
        ("vfx_rain_heavy", "rain_heavy", [0, 0, 0], {}, "rain", [4.5, 1.6, 6], [0, 0.6, 0], 3),
        ("vfx_smoke_column", "smoke_column_gpu", [0, 0, -1], {}, False, [9, 3.5, 13], [0, 5, 0], 10),
    ]
    for name, effect, pos, ov, night, eye, target, warm in shots:
        print(name, flush=True)
        s = Session(name)
        s.call("scene_new", name=name, empty=True)
        stage(s, night)
        if night == "rain":
            s.call("environment_update", **dict(NIGHT, skyTop="#0c1018", skyHorizon="#2a3242", ambient=0.35, fogDensity=0.02))
            for k, (x, z, c) in enumerate([(-2.5, 1.5, "#ffc888"), (2.5, -1.0, "#9ab8ff")]):
                s.call("entity_create", name=f"Street Lamp {k}", position=[x, 3, z],
                       components={"light": {"kind": "point", "color": c, "intensity": 18, "range": 14}})
        elif night:
            s.call("environment_update", **NIGHT)
            s.call("entity_create", name="Lamp", position=[-2.5, 3, 2.5],
                   components={"light": {"kind": "point", "color": "#9ab8ff", "intensity": 6, "range": 12}})
        else:
            s.call("environment_update", skyMode="atmosphere", sunElevation=26, sunAzimuth=35, sunIntensity=2.3, clouds=0.35,
                   tonemap="agx", showGrid=False, ao=1, exposure=1.0, windSpeed=2.5)
        s.call("fx_create", effect=effect, position=pos, overrides=ov)
        s.run(int(warm * 60), every=2, view=(eye, target))
        p = cap(s, name, eye, target, w=1280, h=720, samples=16, fov=45)
        publish(p, name + ".jpg", 960)
        print("  stats:", json.dumps(s.call("fx_stats"))[:300], flush=True)
        s.close()

# --- Benchmark ---------------------------------------------------------------------------------------
if wanted("bench"):
    # GPU timings at 1080p with frames rendered back to back (fx_benchmark), so the GPU runs at
    # full clock like in a game. Numbers are per frame; "frame" is the whole frame command buffer.
    print("bench", flush=True)
    s = Session("bench")
    s.call("scene_new", name="Bench", empty=True)
    stage(s, True)
    s.call("environment_update", **NIGHT)

    def bench(label, eye, target, frames=180):
        st = s.call("fx_benchmark", width=1920, height=1080, frames=frames, eye=eye, target=target)
        print(f"  {label}: frame {st['frameGpuMs']:.2f} ms GPU ({st['wallMsPerFrame']:.2f} ms wall), particle sim "
              f"{st['particlesGpuMs']:.2f} ms, hair sim {st['hairGpuMs']:.2f} ms, hair deep opacity "
              f"{st['hairShadowGpuMs']:.2f} ms", flush=True)
        return st

    far = ([7, 3, 9], [0, 1.5, 0])
    bench("empty stage", *far)
    s.call("fx_create", effect="ember_storm", name="Million",
           overrides={"rate": 250000, "lifetime": 4, "maxParticles": 1000000, "shapeSize": [24, 8, 24], "intensity": 2,
                      "light": 0, "facing": "camera"})
    st = bench("1M particles, additive, curl noise", *far)
    print("    alive:", sum(e["alive"] for e in st["emitters"]), flush=True)
    s.call("entity_update", entity="Million", components={"particles": {"look": "smoke", "sort": True, "sizeStart": 0.05,
                                                                          "sizeEnd": 0.08, "colorStart": "#a0a0a040"}})
    bench("1M particles, lit smoke + GPU bitonic sort", *far)
    s.call("entity_delete", entity="Million")
    bust(s, "hair_wavy", {"strands": 100000})
    close = ([0.45, 1.7, 0.55], [0, 1.6, 0])
    mid = ([1.2, 1.7, 1.5], [0, 1.5, 0])
    st = bench("100k-strand groom, close-up (fills the screen)", *close)
    print("    drawn strands:", st["grooms"][0]["drawn"], flush=True)
    st = bench("100k-strand groom, medium shot", *mid)
    print("    drawn strands:", st["grooms"][0]["drawn"], flush=True)
    s.call("environment_update", windSpeed=6)
    bench("100k-strand groom, close-up, wind", *close)
    s.close()
print("done", flush=True)
