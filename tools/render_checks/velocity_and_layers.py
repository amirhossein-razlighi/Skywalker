#!/usr/bin/env python3
"""Opt-in GPU checks for the velocity buffer and render layers (macOS, Metal). Not part of ctest:
tests must not depend on GPU output. Builds its scenes through MCP tools in a temporary project
(no binary assets), renders small frames and asserts image statistics.

  python3 tools/render_checks/velocity_and_layers.py [--binary build/release/bin/skywalker] [--out DIR]

Checks:
  layers    a red lamp with cullMask = layer 2 lights the cube on layer 2, not the one on layer 1;
            a scene camera with cullMask = layer 1 does not draw the layer-2 cube at all.
  velocity  a fast textured cube and a swinging skinned strip, 40 real-time TAA frames: the
            difference to a 16-sample still of the same instant stays small (no ghost trails);
            debug_view "motion" shows motion on the moving things only.
Needs numpy and Pillow. Exit code 0 = pass.
"""
import argparse, base64, json, math, os, shutil, struct, subprocess, sys, tempfile

import numpy as np
from PIL import Image


class Mcp:
    def __init__(self, binary, project):
        self.p = subprocess.Popen([binary, "mcp", "--project", project], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.id = 0
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "render_checks", "version": "1"}})
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}}) + "\n")
        self.p.stdin.flush()

    def request(self, method, params):
        self.id += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.id, "method": method, "params": params}) + "\n")
        self.p.stdin.flush()
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("skywalker mcp exited")
            msg = json.loads(line)
            if msg.get("id") == self.id:
                if "error" in msg:
                    raise RuntimeError(json.dumps(msg["error"]))
                return msg["result"]

    def call(self, tool, args=None, image=None):
        r = self.request("tools/call", {"name": tool, "arguments": args or {}})
        if r.get("isError"):
            raise RuntimeError(tool + ": " + " ".join(c.get("text", "") for c in r.get("content", [])))
        for c in r.get("content", []):
            if c.get("type") == "image" and image:
                with open(image, "wb") as f:
                    f.write(base64.b64decode(c["data"]))
        return r.get("structuredContent", {})

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=60)


def swing_gltf(path):
    """A 1 x 3 m two-bone strip (with UVs) whose upper bone swings +-60 degrees per second."""
    pos, nrm, uv, joints, weights, idx = [], [], [], [], [], []
    rows = 13
    for r in range(rows):
        y = 3.0 * r / (rows - 1)
        for x in (-0.5, 0.5):
            pos += [x, y, 0.0]
            nrm += [0.0, 0.0, 1.0]
            uv += [x + 0.5, y / 3.0]
            w1 = 0.0 if y < 0.9 else 1.0 if y > 1.1 else (y - 0.9) / 0.2
            joints += [0, 1, 0, 0]
            weights += [1.0 - w1, w1, 0.0, 0.0]
    for r in range(rows - 1):
        a = r * 2
        idx += [a, a + 1, a + 2, a + 1, a + 3, a + 2]
    blob, views, accessors = bytearray(), [], []

    def add(data, fmt, comp, count, typ, minmax=None):
        while len(blob) % 4:
            blob.append(0)
        off = len(blob)
        blob.extend(struct.pack("<%d%s" % (len(data), fmt), *data))
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(blob) - off})
        acc = {"bufferView": len(views) - 1, "componentType": comp, "count": count, "type": typ}
        if minmax:
            acc["min"], acc["max"] = minmax
        accessors.append(acc)
        return len(accessors) - 1

    def quat_z(deg):
        h = math.radians(deg) / 2
        return [0.0, 0.0, math.sin(h), math.cos(h)]

    a_pos = add(pos, "f", 5126, len(pos) // 3, "VEC3", ([-0.5, 0.0, 0.0], [0.5, 3.0, 0.0]))
    a_nrm = add(nrm, "f", 5126, len(nrm) // 3, "VEC3")
    a_uv = add(uv, "f", 5126, len(uv) // 2, "VEC2")
    a_j = add(joints, "H", 5123, len(joints) // 4, "VEC4")
    a_w = add(weights, "f", 5126, len(weights) // 4, "VEC4")
    a_idx = add(idx, "H", 5123, len(idx), "SCALAR")
    a_ibm = add([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1], "f", 5126, 2, "MAT4")
    a_t = add([0.0, 0.5, 1.0], "f", 5126, 3, "SCALAR", ([0.0], [1.0]))
    a_r = add(quat_z(-60) + quat_z(60) + quat_z(-60), "f", 5126, 3, "VEC4")
    gltf = {
        "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "Armature", "children": [1, 3]}, {"name": "Root", "children": [2]},
                  {"name": "Upper", "translation": [0, 1, 0]}, {"name": "Body", "mesh": 0, "skin": 0}],
        "skins": [{"joints": [1, 2], "inverseBindMatrices": a_ibm, "skeleton": 1}],
        "meshes": [{"name": "Swing", "primitives": [{"attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "TEXCOORD_0": a_uv,
                                                                    "JOINTS_0": a_j, "WEIGHTS_0": a_w}, "indices": a_idx, "material": 0}]}],
        "materials": [{"name": "Orange", "doubleSided": True, "pbrMetallicRoughness": {"baseColorFactor": [1.0, 0.35, 0.05, 1.0]}}],
        "animations": [{"name": "Swing", "samplers": [{"input": a_t, "output": a_r, "interpolation": "LINEAR"}],
                        "channels": [{"sampler": 0, "target": {"node": 2, "path": "rotation"}}]}],
        "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode()}],
        "bufferViews": views, "accessors": accessors,
    }
    with open(path, "w") as f:
        json.dump(gltf, f)


def load(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(np.float32)


def check_layers(binary, out, failures):
    proj = os.path.join(out, "layers_project")
    shutil.rmtree(proj, ignore_errors=True)
    os.makedirs(proj)
    m = Mcp(binary, proj)
    m.call("scene_new", {"name": "Layers", "empty": True})
    m.call("environment_update", {"skyMode": "gradient", "skyTop": "#000000", "skyHorizon": "#000000", "ground": "#000000",
                                  "ambient": 0.0, "sunIntensity": 0.0, "showGrid": False, "gi": 0, "ssr": 0, "bloomIntensity": 0,
                                  "vignette": 0, "fogDensity": 0, "reflections": 0})
    m.call("render_layers", {"action": "name", "layer": 2, "name": "hero"})
    m.call("entity_create", {"name": "Prop", "mesh": "cube", "position": [-1.2, 0.5, 0], "color": "#ffffff"})
    m.call("entity_create", {"name": "Hero", "mesh": "cube", "position": [1.2, 0.5, 0], "color": "#ffffff"})
    m.call("render_layers", {"action": "set", "entities": ["Hero"], "layers": "hero"})
    m.call("entity_create", {"name": "Lamp", "position": [0, 0.5, 1.5], "components": {"light": {"kind": "point", "color": "#ff2020",
                                                                                                  "intensity": 8, "range": 8}}})
    m.call("render_layers", {"action": "set", "entities": ["Lamp"], "cull_mask": "hero"})
    m.call("entity_create", {"name": "Cam", "position": [0, 1.2, 5], "rotation": [-8, 0, 0], "components": {"camera": {"fov": 50}}})
    shot = os.path.join(out, "layers_lit.png")
    m.call("viewport_capture", {"view": "scene", "width": 480, "height": 270, "samples": 4, "annotate": False, "overlays": False}, image=shot)
    img = load(shot)
    left, right = img[110:200, 100:200], img[110:200, 280:380]
    lit_hero, lit_prop = right[..., 0].mean(), left[..., 0].mean()
    print(f"layers: red on hero (layer 2) {lit_hero:.1f}, on prop (layer 1) {lit_prop:.1f}")
    if not (lit_hero > 25 and lit_prop < 6):
        failures.append(f"light cullMask: hero {lit_hero:.1f} should be lit, prop {lit_prop:.1f} should stay dark")
    m.call("render_layers", {"action": "set", "entities": ["Cam"], "cull_mask": 1})
    m.call("entity_update", {"entity": "Lamp", "components": {"light": {"cullMask": 1048575}}})
    shot = os.path.join(out, "layers_hidden.png")
    m.call("viewport_capture", {"view": "scene", "width": 480, "height": 270, "samples": 4, "annotate": False, "overlays": False}, image=shot)
    img = load(shot)
    left, right = img[110:200, 100:200], img[110:200, 280:380]
    print(f"layers: camera cullMask 1 -> prop {left.mean():.1f}, hero {right.mean():.1f}")
    if not (left.mean() > 10 and right.mean() < 2):
        failures.append(f"camera cullMask: prop {left.mean():.1f} should be drawn, hero {right.mean():.1f} hidden")
    m.close()


def check_velocity(binary, out, failures):
    proj = os.path.join(out, "velocity_project")
    shutil.rmtree(proj, ignore_errors=True)
    os.makedirs(os.path.join(proj, "chars"))
    swing_gltf(os.path.join(proj, "chars/swing.gltf"))
    m = Mcp(binary, proj)
    m.call("scene_new", {"name": "Velocity", "empty": True})
    m.call("environment_update", {"skyMode": "gradient", "showGrid": False, "taa": True, "bloomIntensity": 0, "vignette": 0, "gi": 0,
                                  "ssr": 0, "fogDensity": 0, "sharpen": 0, "sunElevation": 55})
    m.call("texture_generate", {"kind": "checker", "name": "textures/check", "scale": 8, "color1": "#f2f2f2", "color2": "#202020"})
    m.call("texture_generate", {"kind": "checker", "name": "textures/bluecheck", "scale": 4, "color1": "#2a7fff", "color2": "#ffd040"})
    m.call("entity_create", {"name": "Floor", "mesh": "plane", "scale": [24, 1, 24],
                             "components": {"mesh": {"texture": "textures/check_albedo.png", "tiling": 6, "color": "#ffffff"}}})
    m.call("entity_create", {"name": "Wall", "mesh": "cube", "position": [0, 2, -4], "scale": [16, 4, 0.2],
                             "components": {"mesh": {"texture": "textures/check_albedo.png", "tiling": 4, "color": "#ffffff"}}})
    m.call("entity_create", {"name": "Mover", "mesh": "cube", "position": [0, 0.6, 0], "scale": [1.2, 1.2, 1.2],
                             "components": {"mesh": {"texture": "textures/bluecheck_albedo.png", "color": "#ffffff"}}})
    m.call("behavior_set", {"entity": "Mover", "name": "Slide", "intent": "slide fast and spin",
                            "source": "behavior Slide\n  intent \"slide fast and spin\"\n  on tick\n    self.position.x = -5 + time * 8\n"
                                      "    rotate self by (0, 120 * dt, 0)\n  end\nend\n"})
    m.call("asset_import", {"path": "chars/swing.gltf", "create_entity": "Swing", "position": [3.2, 0, 0.5]})
    m.call("entity_update", {"entity": "Swing", "components": {"mesh": {"material": "", "texture": "textures/check_albedo.png", "tiling": 3,
                                                                        "color": "#ff9050", "doubleSided": True}}})
    m.call("animator_set", {"entity": "Swing", "play": "Swing", "loop": True, "fade": 0})
    m.call("sim_control", {"action": "play"})
    m.call("sim_control", {"action": "pause"})
    cap = {"width": 960, "height": 540, "samples": 1, "annotate": False, "overlays": False, "eye": [0, 3.2, 9], "target": [0.5, 1.2, 0], "fov": 50}
    taa = os.path.join(out, "velocity_taa.png")
    for i in range(40):
        m.call("sim_control", {"action": "step", "ticks": 1})
        m.call("viewport_capture", dict(cap, include_image=i == 39), image=taa if i == 39 else None)
    ref = os.path.join(out, "velocity_ref.png")
    m.call("viewport_capture", dict(cap, samples=16), image=ref)
    m.call("sim_control", {"action": "step", "ticks": 1})
    motion = os.path.join(out, "velocity_motion.png")
    m.call("viewport_capture", dict(cap, debug_view="motion"), image=motion)
    stats = m.call("perf_stats", {}).get("gpu", {}).get("velocity", {})
    m.close()
    a, b = load(taa), load(ref)
    regions = {"cube": (420, 230, 640, 380), "skinned strip": (590, 130, 780, 380)}
    limits = {"cube": 6.5, "skinned strip": 10.0}  # mean |TAA - reference| (0..255); camera-only motion vectors gave ~7.7 / ~13.3
    for name, (x0, y0, x1, y1) in regions.items():
        err = np.abs(a[y0:y1, x0:x1] - b[y0:y1, x0:x1]).mean()
        print(f"velocity: {name} TAA error {err:.2f} (limit {limits[name]})")
        if err > limits[name]:
            failures.append(f"{name}: TAA ghosting error {err:.2f} > {limits[name]}")
    mo = load(motion)
    sat = mo.max(axis=2) - mo.min(axis=2)  # hue = motion; static pixels are gray
    moving = (sat[230:380, 420:640] > 40).mean()
    static = (sat[420:530, 40:300] > 40).mean()
    print(f"velocity: motion view colored fraction cube {moving:.2f}, static floor {static:.3f}; stats {stats}")
    if moving < 0.1 or static > 0.01:
        failures.append(f"debug_view motion: cube colored {moving:.2f} (want > 0.1), static floor {static:.3f} (want ~0)")
    if stats.get("skinnedWithPreviousPose", 0) < 1:
        failures.append("perf_stats velocity.skinnedWithPreviousPose should be >= 1")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(os.path.dirname(__file__), "../../build/release/bin/skywalker"))
    ap.add_argument("--out", default=os.path.join(tempfile.gettempdir(), "sky_render_checks"))
    ap.add_argument("--only", choices=["layers", "velocity"])
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    failures = []
    if args.only in (None, "layers"):
        check_layers(os.path.abspath(args.binary), args.out, failures)
    if args.only in (None, "velocity"):
        check_velocity(os.path.abspath(args.binary), args.out, failures)
    print("images in", args.out)
    if failures:
        print("FAIL:\n  " + "\n  ".join(failures))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
