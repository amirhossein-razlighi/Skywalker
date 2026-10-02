#!/usr/bin/env python3
"""Renders the engine footage for the demo video (real-time engine renders, simulation running).

  render_footage.py VILLAGE_PROJECT DASH_PROJECT OUT_DIR
"""
import math, os, sys, time
from sky import Sky

village, dash, out = sys.argv[1:4]
W, H, FPS = 1920, 1080, 30
TICKS_PER_FRAME = 60 // FPS


def shots(sky, name, n, camera_fn, before_frame=None):
    d = os.path.join(out, name)
    os.makedirs(d, exist_ok=True)
    t0 = time.time()
    for i in range(n):
        if before_frame:
            before_frame(i)
        sky.call("sim_control", action="step", ticks=TICKS_PER_FRAME)
        args = {"width": W, "height": H, "annotate": False, "overlays": False, "include_image": False,
                "save_path": os.path.join(d, f"{i:04d}.png")}
        args.update(camera_fn(i))
        sky.call("viewport_capture", **args)
    print(f"{name}: {n} frames in {time.time() - t0:.1f}s", flush=True)


def orbit(i, n, start_deg, sweep_deg, radius, h0, h1, target=(0, 1.2, 0)):
    t = i / max(n - 1, 1)
    e = t * t * (3 - 2 * t)  # smoothstep
    a = math.radians(start_deg + sweep_deg * e)
    return {"eye": [target[0] + math.sin(a) * radius, h0 + (h1 - h0) * e, target[2] + math.cos(a) * radius],
            "target": list(target)}


def lerp(a, b, t):
    return a + (b - a) * t


def mix_hex(a, b, t):
    ca = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    cb = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join(f"{int(round(lerp(x, y, t))):02x}" for x, y in zip(ca, cb))


# --- Village -------------------------------------------------------------------------
sky = Sky(project=village, scene="scenes/main.sky.json")
sky.call("sim_control", action="play")
sky.call("sim_control", action="step", ticks=120)  # let everything settle into motion

shots(sky, "intro", 225, lambda i: orbit(i, 225, 52, 38, 34, 15, 10))
shots(sky, "hero2", 180, lambda i: orbit(i, 180, 140, 40, 22, 6, 8, target=(0, 1.5, 0)))

# Windmill close-up and airship tracking (Wander behaviors in action)
shots(sky, "windmill", 150, lambda i: {"eye": [-1.5 + i * 0.01, 4.2, 2.5], "target": [-7, 4.0, -6]})


def airship_cam(i):
    st = sky.call("entity_get", entity="Airship 1")
    p = st["components"]["transform"]["position"]
    return {"eye": [p[0] * 1.45, p[1] + 2.5, p[2] * 1.45], "target": [p[0] * 0.6, p[1] - 1, p[2] * 0.6]}


shots(sky, "airship", 150, airship_cam)
shots(sky, "fireflies", 120, lambda i: {"eye": [5.5 - i * 0.012, 1.6, 6.0], "target": [0, 1.0, 0]})

# Agent vision: one annotated capture from the filming camera
sky.call("viewport_capture", width=W, height=H, annotate=True, overlays=False, include_image=False,
         eye=[20, 11, 24], target=[0, 1, 0], save_path=os.path.join(out, "vision.png"))
sky.call("viewport_capture", width=W, height=H, annotate=False, overlays=False, include_image=False,
         eye=[20, 11, 24], target=[0, 1, 0], save_path=os.path.join(out, "vision_clean.png"))

# Golden hour -> night
env0 = sky.call("environment_get")
night = {"skyTop": "#070b1f", "skyHorizon": "#26305a", "ground": "#141629", "sunColor": "#8fa6ff", "fogColor": "#1b2142"}
nums0 = {k: env0[k] for k in ("sunIntensity", "ambient", "fogDensity", "exposure", "sunElevation")}
nums1 = {"sunIntensity": 0.35, "ambient": 0.12, "fogDensity": 0.012, "exposure": 1.35, "sunElevation": 35}
N = 180


def to_night(i):
    t = min(1.0, max(0.0, (i - 20) / (N - 60)))
    t = t * t * (3 - 2 * t)
    patch = {k: mix_hex(env0[k], v, t) for k, v in night.items()}
    patch.update({k: lerp(nums0[k], nums1[k], t) for k in nums0})
    sky.call("environment_update", **patch)


shots(sky, "night", N, lambda i: orbit(i, N, 30, 25, 30, 12, 9), before_frame=to_night)
sky.close()

# --- Sky Dash (2D) -------------------------------------------------------------------
dash_sky = Sky(project=dash, scene="scenes/main.sky.json")
dash_sky.call("sim_control", action="play")
coins = []


def count_coins(i):
    if i % 3 == 0:
        st = dash_sky.call("entity_get", entity="Player")
        coins.append((i, st["vars"].get("coins", 0)))


d = os.path.join(out, "dash")
os.makedirs(d, exist_ok=True)
for i in range(270):
    count_coins(i)
    dash_sky.call("sim_control", action="step", ticks=TICKS_PER_FRAME)
    dash_sky.call("viewport_capture", width=W, height=H, view="scene", annotate=False, overlays=False,
                  include_image=False, save_path=os.path.join(d, f"{i:04d}.png"))
with open(os.path.join(out, "dash_coins.txt"), "w") as f:
    for i, c in coins:
        f.write(f"{i} {c}\n")
print("dash: 270 frames", flush=True)
dash_sky.close()
