#!/usr/bin/env python3
"""Look-dev helper: try camera framings quickly.

    python3 scripts/look.py PROJECT_DIR OUT_PREFIX '[{"eye":[..],"target":[..],"fov":45}, ...]' [--w 960] [--samples 4] [--warmup 60] [--mode final|clay|sketch|all]

Writes OUT_PREFIX_<i>[_mode].png for each view, from one engine session.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render_stills import Sky  # noqa: E402


def opt(name, default):
    return type(default)(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else default


def main():
    proj, prefix, views = sys.argv[1], os.path.abspath(sys.argv[2]), json.loads(sys.argv[3])
    w = opt("--w", 960)
    samples = opt("--samples", 4)
    warm = opt("--warmup", 60)
    mode = opt("--mode", "final")
    modes = ["sketch", "clay", "final"] if mode == "all" else [mode]
    sky = Sky(proj)
    try:
        if warm:
            sky.call("sim_control", action="play")
            sky.call("sim_control", action="step", ticks=warm)
        for i, v in enumerate(views):
            for m in modes:
                t0 = time.time()
                args = dict(width=w, height=w * 9 // 16, annotate=False, overlays=False, include_image=False, samples=samples)
                args.update(v)
                if m == "clay":
                    args["clay"] = True
                elif m == "sketch":
                    args["debug_view"] = "sketch"
                suffix = "" if len(modes) == 1 else f"_{m}"
                args["save_path"] = f"{prefix}_{i}{suffix}.png"
                sky.call("viewport_capture", **args)
                print(f"{i} {m}: {time.time() - t0:.1f}s", flush=True)
    finally:
        sky.close()


if __name__ == "__main__":
    main()
