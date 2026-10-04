#!/usr/bin/env python3
"""Showcase games: build them, look-dev them, render their footage.

  showcase.py list
  showcase.py build GAME [--attach] [--pace S]   build examples/GAME (headless, or in the live editor)
  showcase.py stills GAME OUT_DIR                 one still per shot (look-dev)
  showcase.py footage GAME OUT_DIR [--fps 30]     render every shot as PNG frames

The live editor must have been started with SKY_PROJECT=examples/GAME for --attach.
"""
import importlib
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "games"))
from kit import Studio  # noqa: E402
from sky import ROOT, Sky  # noqa: E402

GAMES = ["hollow_manor", "abyss", "hearthside", "harvest_fair", "neon_drift", "star_lancer",
         "cloudhopper", "toy_kart_rally", "zen_garden", "cyber_alley", "frostlight", "sky_dash",
         "smugglers_cove", "hidden_alley", "namaqua_canyon", "tidebreak_isle", "neon_requiem", "gloamwater", "meridian_accord", "chancellors_desk"]
W, H = 1920, 1080


def module(game):
    return importlib.import_module(game)


def project_dir(game):
    return os.path.join(ROOT, "examples", game)


def build(game, attach=False, pace=0.0, log=None):
    m = module(game)
    d = project_dir(game)
    os.makedirs(os.path.join(d, "scenes"), exist_ok=True)
    t0 = time.time()
    studio = Studio(d, attach=attach, pace=pace, style=getattr(m, "STYLE", None), log=log)
    try:
        m.build(studio)
    finally:
        studio.close()
    meta = dict(m.META)
    meta.pop("view", None)
    with open(os.path.join(d, "game.json"), "w") as f:
        json.dump(meta, f, indent=2)
    print(f"{game}: built in {time.time() - t0:.1f}s", flush=True)


def _open(game):
    return Sky(project=project_dir(game), scene="scenes/main.sky.json")


def render(game, out, fps=30, stills=False, width=W, height=H):
    m = module(game)
    sky = _open(game)
    ticks = max(1, 60 // fps)
    os.makedirs(out, exist_ok=True)
    for shot in m.shots():
        sky.call("sim_control", action="stop")
        sky.call("sim_control", action="play")
        if "start" in shot:  # e.g. start the shot's sequence together with the simulation
            shot["start"](sky)
        warm = shot.get("warmup", 90)
        if warm:
            sky.call("sim_control", action="step", ticks=warm)
        n = shot["frames"]
        d = out if stills else os.path.join(out, shot["name"])
        os.makedirs(d, exist_ok=True)
        t0 = time.time()
        for i in range(n):
            if stills and i != n // 2:
                sky.call("sim_control", action="step", ticks=ticks)
                continue
            if "before" in shot:
                shot["before"](sky, i, n)
            args = dict(width=width, height=height, annotate=False, overlays=False, include_image=False)
            if shot.get("view") == "scene":
                args["view"] = "scene"
            else:
                args.update(shot["cam"](i, n))
            path = os.path.join(d, f"{game}_{shot['name']}.png") if stills else os.path.join(d, f"{i:04d}.png")
            args["save_path"] = path
            sky.call("viewport_capture", **args)
            sky.call("sim_control", action="step", ticks=ticks)
        print(f"{game}/{shot['name']}: {1 if stills else n} frames in {time.time() - t0:.1f}s", flush=True)
    sky.close()


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "list":
        print("\n".join(GAMES))
    elif cmd == "build":
        pace = float(sys.argv[sys.argv.index("--pace") + 1]) if "--pace" in sys.argv else 0.0
        log = sys.argv[sys.argv.index("--log") + 1] if "--log" in sys.argv else None
        build(sys.argv[2], attach="--attach" in sys.argv, pace=pace, log=log)
    elif cmd == "stills":
        render(sys.argv[2], sys.argv[3], stills=True, width=1280, height=720)
    elif cmd == "footage":
        fps = int(sys.argv[sys.argv.index("--fps") + 1]) if "--fps" in sys.argv else 30
        render(sys.argv[2], sys.argv[3], fps=fps)
