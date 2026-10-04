#!/usr/bin/env python3
"""Render the film's moving footage: every slot of src/footage.ts that has a `clip` spec becomes
public/footage/<id>.mp4 (HEVC, 1920x1080, 30 fps) through the engine's Movie Render Queue (`movie_render`).

    python3 scripts/render_footage.py                    # every missing clip, one after the other
    python3 scripts/render_footage.py --only open_hero   # some slots (comma separated ids, or prefixes ending in *)
    python3 scripts/render_footage.py --list             # what would render, and what exists
    python3 scripts/render_footage.py --force --only x   # re-render even if the clip exists
    python3 scripts/render_footage.py --only x --frames 2 --out /tmp/look   # quick framing test (not resumable)
    python3 scripts/render_footage.py --linework         # trace public/linework/<id>.json from each sketch clip's first frame

Resumable: a clip that exists is skipped, and a clip is written to <id>.part.mp4 and renamed only when it is
complete, so an interrupted run never leaves a truncated clip behind. Clips render strictly one at a time
(the engine also takes the machine-wide GPU lock per sub-frame).

A clip spec (see `ClipSpec` in src/footage.ts) renders either a sub-range of one of the scene's own
`*.sequence.json` hero moves (camera and mood come with it), or an inline camera path; `mood` then names a
sequence whose environment keys are applied first, so an inline move gets the same light. Sketch, clay and
final versions of one move (the `triple` slots) line up frame for frame.

Scenes whose big generated assets (downloads, kits, terrain caches) live in another checkout: point at that
project folder with `--project name=/abs/path/examples/name` (repeatable) or SKY_PROJECTS="name=path:name=path";
anything else comes from this repo's examples/.

Env: SKY_CLI (default <repo>/build/release/bin/skywalker), SKY_PROJECTS, SKY_SAMPLES (default 8).
Needs ffmpeg (for --linework).
"""
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render_stills import CLI, FILM, REPO, Sky  # noqa: E402

OUT = os.path.join(FILM, "public", "footage")
FPS = 30
PROJECTS = dict(kv.split("=", 1) for kv in os.environ.get("SKY_PROJECTS", "").split(":") if "=" in kv)
MAX_SAMPLES = 8


def manifest():
    js = subprocess.run(["node", os.path.join(FILM, "scripts", "manifest.mjs")], check=True, capture_output=True, text=True).stdout
    return json.loads(js)


def project_dir(name):
    p = PROJECTS.get(name) or os.path.join(REPO, "examples", name)
    if not os.path.isdir(os.path.join(p, "scenes")):
        raise RuntimeError(f"no project {name} at {p} (pass --project {name}=/path/to/it)")
    return os.path.realpath(p)


def mood_args(project, sequence):
    """environment_update arguments from a sequence's `environment.*` property tracks (their first key)."""
    seq = json.load(open(os.path.join(project, sequence)))
    args = {}
    for tr in seq.get("tracks", []):
        prop = tr.get("property", "")
        if tr.get("type") == "property" and prop.startswith("environment.") and tr.get("keys") and not tr.get("entity"):
            args[prop.split(".", 1)[1]] = tr["keys"][0]["value"]
    return args


def session_key(slot):
    c = slot["clip"]
    return (c.get("project") or slot["scene"], c.get("scene", "scenes/main.sky.json"), c.get("mood"), json.dumps(c.get("setup", [])))


def wanted(slot, only):
    if not only:
        return True
    return any(slot["id"] == o or (o.endswith("*") and slot["id"].startswith(o[:-1])) for o in only)


def render(sky, project, slot, samples, out_dir=OUT, frames=None):
    c = slot["clip"]
    out = os.path.join(out_dir, slot["id"] + ".mp4")
    part = os.path.join(out_dir, slot["id"] + ".part.mp4")
    if os.path.exists(part):
        os.remove(part)
    w, h = slot["resolution"]
    args = dict(output=part, codec="hevc", width=w, height=h, fps=FPS, samples=min(samples, c.get("samples", samples), MAX_SAMPLES),
                shutter=c.get("shutter", 0.5), simulate=c.get("simulate", True), duration=(frames or slot["frames"]) / FPS)
    if c.get("sequence"):
        args["sequence"] = c["sequence"]
        args["start"] = c.get("start", 0)
    elif c.get("camera"):
        args["camera"] = c["camera"]
        if c.get("start"):
            args["start"] = c["start"]
    if slot["mode"] == "clay":
        args["clay"] = True
    elif slot["mode"] == "sketch":
        args["debug_view"] = "sketch"
    t0 = time.time()
    res = sky.call("movie_render", **args)
    if not os.path.exists(part):
        raise RuntimeError(f"no output ({res})")
    os.replace(part, out)
    frames = res.get("frames") if isinstance(res, dict) else "?"
    print(f"  {slot['id']}: {frames} frames, {os.path.getsize(out) / 1e6:.1f} MB in {time.time() - t0:.0f}s", flush=True)


def linework():
    """public/linework/<id>.json for every sketch clip, traced from its first frame (the reveal holds on it while the
    strokes draw, then the clip starts to move)."""
    import shutil
    import tempfile
    tmp = tempfile.mkdtemp(prefix="film-linework-")
    for f in sorted(os.listdir(OUT)):
        if f.endswith("_sketch.mp4"):
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", os.path.join(OUT, f), "-frames:v", "1", os.path.join(tmp, f[:-4] + ".png")], check=True)
    subprocess.run([sys.executable, os.path.join(FILM, "scripts", "trace_lines.py"), tmp, os.path.join(FILM, "public", "linework")], check=True)
    shutil.rmtree(tmp, ignore_errors=True)


def main():
    argv = sys.argv[1:]
    for i, a in enumerate(argv[:-1]):
        if a == "--project" and "=" in argv[i + 1]:
            k, v = argv[i + 1].split("=", 1)
            PROJECTS[k] = v
    if "--linework" in argv:
        linework()
        return
    test_frames = int(argv[argv.index("--frames") + 1]) if "--frames" in argv else None
    out_dir = os.path.abspath(argv[argv.index("--out") + 1]) if "--out" in argv else OUT
    only = argv[argv.index("--only") + 1].split(",") if "--only" in argv else None
    force = "--force" in argv
    samples = int(os.environ.get("SKY_SAMPLES", MAX_SAMPLES))
    slots = [s for s in manifest() if s.get("clip") and not s.get("pending") and wanted(s, only)]
    todo = [s for s in slots if force or out_dir != OUT or not os.path.exists(os.path.join(OUT, s["id"] + ".mp4"))]
    todo.sort(key=lambda s: session_key(s)[0])  # stable: one engine load per project
    if "--list" in argv:
        for s in slots:
            state = "todo" if s in todo else "done"
            print(f"{state:5} {s['id']:24} {s['scene']:16} {s['mode']:6} {s['frames']:4}f  {s['clip'].get('sequence') or 'inline camera'}")
        print(f"{len(todo)} to render, {sum(s['frames'] for s in todo)} frames")
        return
    os.makedirs(out_dir, exist_ok=True)
    print(f"{len(todo)} clips, {sum(s['frames'] for s in todo)} frames, CLI {CLI}", flush=True)
    failed = []
    sky, key = None, None
    try:
        for s in todo:
            k = session_key(s)
            try:
                if k != key:
                    if sky:
                        sky.close()
                    sky, key = None, None
                    project = project_dir(k[0])
                    print(f"[{k[0]} {k[1]}{' mood ' + k[2] if k[2] else ''}]", flush=True)
                    sky = Sky(project, k[1])
                    key = k
                    if k[2]:
                        sky.call("environment_update", **mood_args(project, k[2]))
                    for op in json.loads(k[3]):
                        sky.call(op["tool"], **op["args"])
                render(sky, project, s, samples, out_dir, test_frames)
            except Exception as e:  # keep going: one broken scene should not stop the batch
                failed.append(s["id"])
                print(f"  {s['id']}: FAILED {e}", flush=True)
                if sky:
                    sky.close()
                sky, key = None, None
    finally:
        if sky:
            sky.close()
    if failed:
        print("failed:", ",".join(failed))
        sys.exit(1)


if __name__ == "__main__":
    main()
