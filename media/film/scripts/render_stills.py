#!/usr/bin/env python3
"""Render the film's footage stills from the manifest (src/footage.ts) through the Skywalker CLI.

    python3 scripts/render_stills.py MANIFEST.json [--only id,id] [--samples 16] [--skip-existing]

Each slot with a `still` spec is rendered once per mode (final / clay / sketch) to public/footage/<id>.png.
The engine runs headless over MCP (`skywalker mcp`), so the simulation can warm up (fire, particles, behaviors)
before the capture. Photoscanned examples need their Poly Haven downloads: if examples/<name>/downloads is
missing, set SKY_ASSETS_FROM to another checkout's examples/ folder and the script stages a copy that links them.

Env: SKY_CLI (default <repo>/build/release/bin/skywalker), SKY_EXAMPLES (default <repo>/examples), SKY_ASSETS_FROM,
SKY_STAGE (keep staged copies in this folder between runs instead of a temp folder).
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FILM = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(FILM))
CLI = os.environ.get("SKY_CLI", os.path.join(REPO, "build", "release", "bin", "skywalker"))
EXAMPLES = os.environ.get("SKY_EXAMPLES", os.path.join(REPO, "examples"))
ASSETS_FROM = os.environ.get("SKY_ASSETS_FROM")
OUT = os.path.join(FILM, "public", "footage")
LINKED = ("downloads", "terrain")


class Sky:
    """Minimal MCP stdio client (same protocol as media/demo/sky.py)."""

    def __init__(self, project, scene="scenes/main.sky.json"):
        self.n = 0
        # realpath: the engine resolves asset paths against the literal --project string, so relative or symlinked
        # project paths make photoscan assets fail to load.
        self.proc = subprocess.Popen([CLI, "mcp", "--project", os.path.realpath(project), "--scene", scene],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.rpc("initialize", {"protocolVersion": "2025-11-25", "clientInfo": {"name": "film-stills"}})

    def rpc(self, method, params):
        self.n += 1
        self.proc.stdin.write((json.dumps({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params}) + "\n").encode())
        self.proc.stdin.flush()
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("engine closed the connection")
            msg = json.loads(line)
            if msg.get("id") == self.n:
                if "error" in msg:
                    raise RuntimeError(msg["error"]["message"])
                return msg["result"]

    def call(self, tool, **args):
        r = self.rpc("tools/call", {"name": tool, "arguments": args})
        if r.get("isError"):
            raise RuntimeError(f"{tool}: {r['content'][0]['text']}")
        return r.get("structuredContent")

    def close(self):
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.proc.kill()


_staged = {}
STAGE = os.environ.get("SKY_STAGE")


def project_dir(name):
    """The example folder, or a staged copy with photoscan downloads cloned from SKY_ASSETS_FROM.

    The engine only reads assets that really live inside the project (symlinks out of it are refused), so the
    staged copy clones them (APFS copy-on-write via `cp -c`: instant, no extra disk space)."""
    src = os.path.join(EXAMPLES, name)
    if not ASSETS_FROM:
        return src
    need = [d for d in LINKED if os.path.exists(os.path.join(ASSETS_FROM, name, d)) and not os.path.exists(os.path.join(src, d))]
    if not need:
        return src
    if name not in _staged:
        root = STAGE or tempfile.mkdtemp(prefix="film-stills-")
        dst = os.path.join(root, name)
        if os.path.islink(dst) or not os.path.isdir(dst):
            shutil.copytree(src, dst, ignore=shutil.ignore_patterns(*LINKED), dirs_exist_ok=True)
        for d in need:
            target = os.path.join(dst, d)
            if os.path.islink(target):
                os.unlink(target)
            if not os.path.exists(target):
                subprocess.run(["cp", "-Rc", os.path.join(ASSETS_FROM, name, d), target], check=True)
        _staged[name] = dst
    return _staged[name]


def render_slot(slot, samples, skip_existing):
    st = slot["still"]
    out = os.path.join(OUT, slot["id"] + ".png")
    if skip_existing and os.path.exists(out):
        print(f"  {slot['id']}: exists, skipped")
        return
    if st.get("copyFrom"):
        src = os.path.join(REPO, st["copyFrom"])
        subprocess.run(["sips", "-s", "format", "png", src, "--out", out], check=True, stdout=subprocess.DEVNULL)
        print(f"  {slot['id']}: copied {st['copyFrom']}")
        return
    proj = project_dir(slot["scene"])
    t0 = time.time()
    sky = Sky(proj)
    try:
        for op in st.get("setup", []):
            sky.call(op["tool"], **op["args"])
        warm = st.get("warmup", 60)
        if warm:
            sky.call("sim_control", action="play")
            sky.call("sim_control", action="step", ticks=warm)
        w, h = slot["resolution"]
        args = dict(width=w, height=h, annotate=False, overlays=False, include_image=False,
                    samples=st.get("samples", samples), save_path=out)
        if st.get("sceneCamera"):
            args["view"] = "scene"
        else:
            args.update(eye=st["eye"], target=st["target"])
            if "fov" in st:
                args["fov"] = st["fov"]
        if slot["mode"] == "clay":
            args["clay"] = True
        elif slot["mode"] == "sketch":
            args["debug_view"] = "sketch"
        res = sky.call("viewport_capture", **args)
        if st.get("marks") and res:
            with open(os.path.join(OUT, slot["id"] + ".marks.json"), "w") as f:
                json.dump({"width": w, "height": h, "visible": res.get("visible", [])}, f, indent=1)
    finally:
        sky.close()
    print(f"  {slot['id']}: {slot['scene']} {slot['mode']} in {time.time() - t0:.1f}s", flush=True)


def main():
    manifest = json.load(open(sys.argv[1]))
    only = set(sys.argv[sys.argv.index("--only") + 1].split(",")) if "--only" in sys.argv else None
    samples = int(sys.argv[sys.argv.index("--samples") + 1]) if "--samples" in sys.argv else 16
    skip = "--skip-existing" in sys.argv
    os.makedirs(OUT, exist_ok=True)
    failed = []
    for slot in manifest:
        if not slot.get("still") or slot.get("pending"):
            continue
        if only and slot["id"] not in only:
            continue
        try:
            render_slot(slot, samples, skip)
        except Exception as e:  # keep going: one broken scene should not stop the batch
            failed.append(slot["id"])
            print(f"  {slot['id']}: FAILED {e}", flush=True)
    if not STAGE:
        for d in _staged.values():
            shutil.rmtree(os.path.dirname(d), ignore_errors=True)
    if failed:
        print("failed:", ",".join(failed))
        sys.exit(1)


if __name__ == "__main__":
    main()
