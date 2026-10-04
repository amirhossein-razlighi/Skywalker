#!/usr/bin/env python3
"""Build the site's images and clips into website/docs/assets.

    python3 website/scripts/make_media.py convert                       # reuse existing shots, previews, docs images
    python3 website/scripts/make_media.py render --cli build/release/bin/skywalker [--only NAME,...]
                                                                        # render new stills with the engine

convert  examples/*/shots/*.jpg      -> assets/images/shots/<example>/<shot>.webp   (1280 px wide, quality 82)
         examples/*/previews/*.mp4   -> assets/video/<example>/<clip>.mp4 (H.264, 720p, <= 8 s) + .webp poster
         docs/images/{hair,fur}_*    -> assets/images/hair/*.webp,  docs/images/vfx_* -> assets/images/vfx/*.webp
render   drives `skywalker mcp` (headless) to build small look-dev scenes from the engine's own presets and
         captures them with viewport_capture (1280x720, at most 8 samples, one render at a time; the CLI holds the
         machine-wide GPU lock while it renders). Scratch files go to the system temp folder.

Every output is stripped of metadata (no EXIF, no encoder tags with paths). Needs Pillow and ffmpeg (libx264).
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "website" / "docs" / "assets"
W, H, SAMPLES = 1280, 720, 8


def to_webp(src: Path, dst: Path, width: int = 1280, quality: int = 82) -> None:
    from PIL import Image
    dst.parent.mkdir(parents=True, exist_ok=True)
    with Image.open(src) as im:
        im = im.convert("RGB")
        if im.width > width:
            im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
        im.save(dst, "WEBP", quality=quality, method=6)  # Pillow writes no EXIF unless asked


def to_mp4(src: Path, dst: Path, seconds: float = 8.0, height: int = 720) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(src), "-t", str(seconds), "-an", "-map_metadata", "-1",
                    "-vf", f"scale=-2:{height}", "-c:v", "libx264", "-preset", "slow", "-crf", "26", "-maxrate", "2.6M",
                    "-bufsize", "5M", "-pix_fmt", "yuv420p", "-profile:v", "high", "-movflags", "+faststart",
                    "-metadata", "encoder=", str(dst)], check=True)
    poster = dst.with_suffix(".webp")
    with tempfile.TemporaryDirectory() as tmp:
        frame = Path(tmp) / "poster.png"
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss", "0.5", "-i", str(dst), "-frames:v", "1", str(frame)], check=True)
        to_webp(frame, poster)


def convert() -> None:
    for shot in sorted(ROOT.glob("examples/*/shots/*.jpg")):
        if shot.stem == "contact_sheet":
            continue
        to_webp(shot, ASSETS / "images" / "shots" / shot.parent.parent.name / f"{shot.stem}.webp")
    for clip in sorted(ROOT.glob("examples/*/previews/*.mp4")):
        to_mp4(clip, ASSETS / "video" / clip.parent.parent.name / f"{clip.stem}.mp4")
    for img in sorted((ROOT / "docs" / "images").glob("*.jpg")):
        group = "vfx" if img.stem.startswith("vfx_") else "hair"
        to_webp(img, ASSETS / "images" / group / f"{img.stem}.webp")
    social_card()
    print("converted existing media")


def social_card() -> None:
    """1200x630 link-preview card: an example shot, darkened, with the horizontal logo (needs rsvg-convert, magick)."""
    with tempfile.TemporaryDirectory() as tmp:
        logo = Path(tmp) / "logo.png"
        subprocess.run(["rsvg-convert", "-w", "760", str(ROOT / "assets/brand/logo/horizontal-dark.svg"), "-o", str(logo)],
                       check=True)
        subprocess.run(["magick", str(ROOT / "examples/tidebreak_isle/shots/establishing.jpg"), "-resize", "1200x630^",
                        "-gravity", "center", "-extent", "1200x630", "(", "-size", "1200x630",
                        "gradient:#07091900-#070919f0", ")", "-compose", "over", "-composite", str(logo), "-gravity",
                        "center", "-compose", "over", "-composite", "-strip", "-quality", "82",
                        str(ASSETS / "images" / "social-card.jpg")], check=True)


# ---------------------------------------------------------------------------------------------------------------
# Rendering through the MCP server


class Mcp:
    def __init__(self, cli: str, project: Path, scene: Path | None = None):
        args = [cli, "mcp", "--project", str(project)]
        if scene:
            args += ["--scene", str(scene)]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                     text=True, cwd=str(project))
        self.next_id = 0
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "site-media", "version": "1"}})
        self.notify("notifications/initialized")

    def notify(self, method: str, params: dict | None = None) -> None:
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method, "params": params or {}}) + "\n")
        self.proc.stdin.flush()

    def request(self, method: str, params: dict):
        self.next_id += 1
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params}) + "\n")
        self.proc.stdin.flush()
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("MCP server exited")
            msg = json.loads(line)
            if msg.get("id") == self.next_id:
                if "error" in msg:
                    raise RuntimeError(f"{method}: {msg['error']}")
                return msg["result"]

    def call(self, tool: str, args: dict | None = None):
        result = self.request("tools/call", {"name": tool, "arguments": args or {}})
        if result.get("isError"):
            text = " ".join(c.get("text", "") for c in result.get("content", []))
            raise RuntimeError(f"{tool} failed: {text}")
        return result.get("structuredContent", result)

    def close(self) -> None:
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=30)
        except Exception:
            self.proc.kill()


def capture(mcp: Mcp, out: Path, **view) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        png = Path(tmp) / "shot.png"
        args = {"width": W, "height": H, "samples": SAMPLES, "annotate": False, "overlays": False,
                "include_image": False, "save_path": str(png)}
        args.update(view)
        mcp.call("viewport_capture", args)
        to_webp(png, out)


def batch(mcp: Mcp, ops: list[tuple[str, dict]]) -> None:
    mcp.call("batch", {"operations": [{"tool": t, "args": a} for t, a in ops]})


def env(mcp: Mcp, **fields) -> None:
    mcp.call("environment_update", fields)


def scene_lookdev(mcp: Mcp) -> None:
    """A small courtyard: textured floor and walls, spheres in material presets, a lamp, a tree-like prop."""
    mcp.call("scene_new", {"name": "Lookdev", "empty": True})
    env(mcp, preset="sunset", skyMode="atmosphere", clouds=0.35, gi=1, ssr=1, ao=1, tonemap="agx", showGrid=False,
        sunElevation=24, sunAzimuth=230)
    mcp.call("texture_generate", {"kind": "cobblestone", "name": "cobble", "create_material": True})
    mcp.call("texture_generate", {"kind": "bricks", "name": "bricks", "create_material": True})
    mcp.call("texture_generate", {"kind": "planks", "name": "planks", "create_material": True})
    ops = [
        ("entity_create", {"name": "Floor", "mesh": "plane", "scale": [24, 1, 24]}),
        ("entity_create", {"name": "Back Wall", "mesh": "cube", "position": [0, 2, -6], "scale": [14, 4, 0.6]}),
        ("entity_create", {"name": "Side Wall", "mesh": "cube", "position": [-6.5, 1.5, -2], "scale": [0.6, 3, 8]}),
        ("entity_create", {"name": "Plinth", "mesh": "cube", "position": [3.5, 0.4, -3], "scale": [2.4, 0.8, 1.6]}),
        ("entity_create", {"name": "Lamp", "position": [-3, 2.6, -2],
                           "components": {"light": {"kind": "point", "color": "#ffb36b", "intensity": 6, "range": 7}}}),
        ("entity_create", {"name": "Lamp Bulb", "mesh": "sphere", "position": [-3, 2.6, -2], "scale": [0.18, 0.18, 0.18],
                           "components": {"mesh": {"emissive": "#ffb36bff", "color": "#ffe2c0"}}}),
        ("entity_create", {"name": "Camera", "position": [0, 2.1, 6.5], "rotation": [-9, 0, 0],
                           "components": {"camera": {"primary": True, "fov": 50}}}),
    ]
    batch(mcp, ops)
    mcp.call("material_assign", {"entities": ["Floor"], "material": "materials/cobble.mat.json"})
    mcp.call("material_assign", {"entities": ["Back Wall", "Side Wall"], "material": "materials/bricks.mat.json"})
    mcp.call("material_assign", {"entities": ["Plinth"], "material": "materials/planks.mat.json"})
    presets = ["gold", "chrome", "car_paint", "glass", "ceramic", "velvet"]
    for i, p in enumerate(presets):
        mcp.call("material_create", {"path": f"materials/{p}.mat.json", "preset": p})
        x = -3.75 + i * 1.5
        mcp.call("entity_create", {"name": f"Ball {p}", "mesh": "sphere", "position": [x, 0.55, 0.5], "scale": [1, 1, 1]})
        mcp.call("material_assign", {"entities": [f"Ball {p}"], "material": f"materials/{p}.mat.json"})


def render_lookdev(mcp: Mcp, out: Path) -> None:
    scene_lookdev(mcp)
    view = {"view": "scene"}
    capture(mcp, out / "debug-final.webp", **view)
    for v in ["albedo", "normals", "material", "gi", "reflections", "ao", "depth", "lighting_only", "unshaded",
              "wireframe", "overdraw", "lod", "shadow_cascades", "light_complexity", "uv_checker", "texel_density",
              "sketch", "emission", "specular"]:
        capture(mcp, out / f"debug-{v}.webp", debug_view=v, **view)
    capture(mcp, out / "debug-clay.webp", clay=True, **view)
    for preset in ["noon", "sunset", "night", "overcast"]:
        env(mcp, preset=preset, skyMode="atmosphere", showGrid=False)
        capture(mcp, out / f"tod-{preset}.webp", **view)
    env(mcp, preset="sunset", skyMode="atmosphere", clouds=0.35, showGrid=False)
    for look in ["none", "teal_orange", "noir", "golden_hour", "vintage", "moonlight"]:
        env(mcp, look=look, lookStrength=1)
        capture(mcp, out / f"look-{look}.webp", **view)
    env(mcp, look="none")
    capture(mcp, out / "dof.webp", aperture=1.4, focus_distance=6.2, **view)


def render_materials(mcp: Mcp, out: Path) -> None:
    mcp.call("scene_new", {"name": "Materials", "empty": True})
    env(mcp, preset="studio", showGrid=False, gi=1, ssr=1, tonemap="agx", bloomIntensity=0.15, exposureCompensation=-0.4)
    presets = ["gold", "silver", "copper", "chrome", "brushed_steel", "iron", "plastic", "rubber", "ceramic",
               "car_paint", "glass", "ice", "skin", "wax", "velvet", "neon", "toon", "clay"]
    mcp.call("entity_create", {"name": "Floor", "mesh": "plane", "scale": [40, 1, 40], "color": "#2a2c34", "components": {"mesh": {"roughness": 0.6}}})
    for i, p in enumerate(presets):
        mcp.call("material_create", {"path": f"materials/{p}.mat.json", "preset": p})
        col, row = i % 6, i // 6
        mcp.call("entity_create", {"name": p, "mesh": "sphere", "position": [-3.75 + col * 1.5, 0.5, -row * 1.6]})
        mcp.call("material_assign", {"entities": [p], "material": f"materials/{p}.mat.json"})
    capture(mcp, out / "material-presets.webp", eye=[0, 3.4, 5.2], target=[0, 0.3, -1.6], fov=48)
    mcp.call("scene_new", {"name": "Textures", "empty": True})
    env(mcp, preset="noon", showGrid=False, skyMode="atmosphere", tonemap="agx")
    kinds = ["bricks", "planks", "cobblestone", "rock", "marble", "rust", "tiles", "hexagons", "scales", "fabric",
             "metal_brushed", "wood"]
    mcp.call("entity_create", {"name": "Floor", "mesh": "plane", "scale": [40, 1, 40], "color": "#3a3d46"})
    for i, k in enumerate(kinds):
        mcp.call("texture_generate", {"kind": k, "name": k, "create_material": True})
        col, row = i % 6, i // 6
        mcp.call("entity_create", {"name": k, "mesh": "cube", "position": [-5.5 + col * 2.2, 0.75, -row * 2.4], "scale": [1.5, 1.5, 1.5]})
        mcp.call("material_assign", {"entities": [k], "material": f"materials/{k}.mat.json"})
    capture(mcp, out / "textures-generated.webp", eye=[0, 6, 8], target=[0, 0.5, -1.2], fov=50)


def render_shading(mcp: Mcp, out: Path) -> None:
    mcp.call("scene_new", {"name": "Shading", "empty": True})
    env(mcp, preset="noon", showGrid=False, skyMode="atmosphere", tonemap="agx", exposureCompensation=-0.9, clouds=0.3)
    ops = [("entity_create", {"name": "Ground", "mesh": "cylinder", "position": [0, -0.25, 0], "scale": [8, 0.5, 8], "color": "#7fb069"}),
           ("entity_create", {"name": "Tower", "mesh": "cylinder", "position": [-1.6, 1.2, -0.8], "scale": [1, 2.4, 1], "color": "#f2e8d5"}),
           ("entity_create", {"name": "Roof", "mesh": "cone", "position": [-1.6, 2.9, -0.8], "scale": [1.5, 1, 1.5], "color": "#e8656f"}),
           ("entity_create", {"name": "Ring", "mesh": "torus", "position": [1.4, 0.9, 0.2], "rotation": [70, 0, 20], "color": "#8f7cf0"}),
           ("entity_create", {"name": "Hero", "mesh": "capsule", "position": [0.2, 0.55, 1.3], "scale": [0.6, 0.6, 0.6], "color": "#3cc9b0"}),
           ("entity_create", {"name": "Coin", "mesh": "torus", "position": [1.9, 1.9, -1.4], "scale": [0.4, 0.4, 0.4], "color": "#f5c542",
                              "components": {"mesh": {"metallic": 1, "roughness": 0.3}}})]
    batch(mcp, ops)
    view = {"eye": [0, 3.2, 6.5], "target": [0, 0.9, 0], "fov": 45}
    capture(mcp, out / "shading-pbr.webp", **view)
    for name in ["Ground", "Tower", "Roof", "Ring", "Hero", "Coin"]:
        mcp.call("entity_update", {"entity": name, "components": {"mesh": {"shading": "toon", "outline": 2.5, "rim": 0.4}}})
    env(mcp, tonemap="neutral", saturation=1.1)
    capture(mcp, out / "shading-toon.webp", **view)


def ground(mcp: Mcp, terrain: str, x: float, z: float) -> float:
    return mcp.call("terrain_query", {"entity": terrain, "points": [[x, z]]})["points"][0]["height"]


def render_world(mcp: Mcp, out: Path) -> None:
    clear = {"fogDensity": 0.0006, "haze": 0.003, "showGrid": False, "tonemap": "agx", "skyMode": "atmosphere"}
    mcp.call("scene_new", {"name": "Island", "empty": True})
    env(mcp, preset="sunset", clouds=0.45, sunElevation=16, sunAzimuth=230, **clear)
    mcp.call("terrain_create", {"name": "Island", "preset": "island_beach", "size": 600, "water": True, "seed": 3})
    mcp.call("foliage_add", {"entity": "Island", "layers": [{"preset": "meadow_grass"}, {"preset": "flowers"},
                                                           {"preset": "beach_pebbles"}]})
    capture(mcp, out / "terrain-island.webp", eye=[-300, 110, 300], target=[0, 0, 0], fov=45)
    h = ground(mcp, "Island", -12, 18)
    capture(mcp, out / "foliage-meadow.webp", eye=[-12, h + 1.7, 18], target=[30, h - 2, -30], fov=60)
    env(mcp, clouds=0.62, sunElevation=22)
    capture(mcp, out / "clouds.webp", eye=[-12, h + 2, 18], target=[200, 420, -600], fov=75)
    mcp.call("scene_new", {"name": "Alpine", "empty": True})
    env(mcp, preset="noon", clouds=0.4, sunElevation=30, sunAzimuth=140, **clear)
    r = mcp.call("terrain_create", {"name": "Valley", "preset": "mountain_valley", "size": 2000, "seed": 5})
    top = r["stats"]["maxHeight"]
    capture(mcp, out / "terrain-alpine.webp", eye=[-1000, top * 1.1, 1000], target=[0, top * 0.25, 0], fov=50)
    mcp.call("scene_new", {"name": "Dunes", "empty": True})
    env(mcp, preset="sunset", clouds=0.1, sunElevation=10, sunAzimuth=250, **clear)
    r = mcp.call("terrain_create", {"name": "Dunes", "preset": "desert_dunes", "size": 800, "seed": 2})
    top = r["stats"]["maxHeight"]
    capture(mcp, out / "terrain-desert.webp", eye=[-380, top + 60, 380], target=[0, top * 0.3, 0], fov=50)
    for preset, name in [("ocean", "water-ocean"), ("storm", "water-storm")]:
        mcp.call("scene_new", {"name": name, "empty": True})
        if preset == "storm":
            env(mcp, preset="overcast", clouds=0.85, windSpeed=14, **clear)
        else:
            env(mcp, preset="sunset", clouds=0.35, sunElevation=12, sunAzimuth=180, **clear)
        mcp.call("fx_create", {"effect": preset, "name": "Sea"})
        capture(mcp, out / f"{name}.webp", eye=[0, 5, 0], target=[0, 1, -60], fov=60)


def render_vfx(mcp: Mcp, out: Path) -> None:
    mcp.call("scene_new", {"name": "Campfire", "empty": True})
    env(mcp, preset="night", skyMode="atmosphere", stars=0.7, showGrid=False, tonemap="agx")
    mcp.call("texture_generate", {"kind": "dirt", "name": "dirt", "create_material": True})
    mcp.call("entity_create", {"name": "Ground", "mesh": "plane", "scale": [30, 1, 30]})
    mcp.call("material_assign", {"entities": ["Ground"], "material": "materials/dirt.mat.json"})
    for i in range(7):
        import math
        a = i / 7 * 2 * math.pi
        mcp.call("entity_create", {"name": f"Stone {i}", "mesh": "sphere", "position": [math.cos(a) * 0.75, 0.12, math.sin(a) * 0.75],
                                   "scale": [0.35, 0.25, 0.3], "color": "#6b6660"})
    mcp.call("fx_create", {"effect": "campfire", "name": "Campfire", "position": [0, 0, 0]})
    mcp.call("sim_control", {"action": "step", "ticks": 90})
    capture(mcp, out / "fx-campfire.webp", eye=[0, 1.6, 4.2], target=[0, 0.8, 0], fov=45)
    mcp.call("sim_control", {"action": "stop"})


def render_agents(mcp: Mcp, out: Path, project: Path) -> None:
    mcp.call("scene_load", {"path": "scenes/main.sky.json"})
    capture(mcp, out / "annotated-capture.webp", annotate=True, view="scene")
    with tempfile.TemporaryDirectory() as tmp:
        result = mcp.request("tools/call", {"name": "viewport_multi", "arguments": {"size": 1280}})
        import base64
        for c in result.get("content", []):
            if c.get("type") == "image":
                png = Path(tmp) / "multi.png"
                png.write_bytes(base64.b64decode(c["data"]))
                to_webp(png, out / "viewport-multi.webp")
                break


JOBS = ["lookdev", "materials", "shading", "world", "vfx", "agents", "examples"]


def render(cli: str, only: set[str]) -> None:
    images = ASSETS / "images"
    with tempfile.TemporaryDirectory() as tmp:
        project = Path(tmp) / "media_project"
        project.mkdir()
        (project / "scenes").mkdir()
        mcp = Mcp(cli, project)
        try:
            if "lookdev" in only:
                render_lookdev(mcp, images / "rendering")
            if "materials" in only:
                render_materials(mcp, images / "rendering")
            if "shading" in only:
                render_shading(mcp, images / "rendering")
            if "world" in only:
                render_world(mcp, images / "world")
            if "vfx" in only:
                render_vfx(mcp, images / "vfx")
        finally:
            mcp.close()
        if "agents" in only:
            demo = Path(tmp) / "hello_sky"
            shutil.copytree(ROOT / "examples" / "hello_sky", demo)
            mcp = Mcp(cli, demo)
            try:
                render_agents(mcp, images / "agents", demo)
            finally:
                mcp.close()
    if "examples" in only:
        render_examples(cli)
    print("rendered:", ", ".join(sorted(only)))


# Examples without shots in the repository: (example, mode). "still" = the scene camera; "run" = simulate first
# (cameras that follow a player need a few ticks).
EXAMPLE_STILLS = [("abyss", "still"), ("cyber_alley", "still"), ("frostlight", "still"), ("harvest_fair", "still"),
                  ("hearthside", "still"), ("hollow_manor", "still"), ("zen_garden", "still"), ("toy_kart_rally", "run"),
                  ("star_lancer", "still"), ("neon_drift", "still"), ("cloudhopper", "run"), ("sky_dash", "still"),
                  ("sky_village", "still"), ("hello_sky", "still")]


def render_examples(cli: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        for name, mode in EXAMPLE_STILLS:
            scene = ROOT / "examples" / name / "scenes" / "main.sky.json"
            png = Path(tmp) / f"{name}.png"
            if mode == "run":
                cmd = [cli, "run", str(scene), "--ticks", "90", "-o", str(png)]
            else:
                cmd = [cli, "render", str(scene), "-o", str(png), "--width", str(W), "--height", str(H),
                       "--samples", str(SAMPLES), "--scene-camera"]
            subprocess.run(cmd, check=True, capture_output=True, cwd=tmp)
            to_webp(png, ASSETS / "images" / "examples" / f"{name}.webp")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["convert", "render"])
    ap.add_argument("--cli", help="skywalker binary (render)")
    ap.add_argument("--only", help=f"comma-separated subset of {','.join(JOBS)}")
    opts = ap.parse_args()
    if opts.action == "convert":
        convert()
    else:
        if not opts.cli:
            ap.error("render needs --cli")
        only = set(opts.only.split(",")) if opts.only else set(JOBS)
        render(str(Path(opts.cli).resolve()), only)
    return 0


if __name__ == "__main__":
    sys.exit(main())
