#!/usr/bin/env python3
"""Quality gate for a showcase project: contact sheet + scene_audit + perf_stats -> PASS / FAIL.

    python3 media/demo/showcase_gate.py PROJECT [--scene scenes/main.sky.json]
            [--sequences sequences/a.sequence.json,sequences/b.sequence.json] [--frames 12]
            [--width 1920 --height 1080 --samples 16] [--out shots] [--stylized] [--no-strict]
            [--preview]   # 1280x720, 8 samples: quick iterations

For each of `frames` moments spread across the sequences (default: every *.sequence.json played by an
entity of the scene), it renders the sequence's live camera (sequence keys and animation applied),
audits the same view with scene_audit (strict by default: primitives or default materials above
0.1% fail), and benchmarks the hero views with perf_stats (frame time, gpuFaults must stay 0).
Writes PROJECT/shots/shot_NN.jpg (<= 400 KB each), shots/contact_sheet.jpg (<= 1.5 MB) and
shots/gate_report.json, prints a summary and exits 0 on PASS, 1 on FAIL.
"""
import argparse
import glob
import io
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from sky import Sky  # noqa: E402


def jpeg(src_png, dst, max_bytes, width=None):
    """Saves a PNG as JPEG under max_bytes (lowering quality, then size). Needs Pillow."""
    from PIL import Image
    im = Image.open(src_png).convert("RGB")
    if width and im.width > width:
        im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
    for q in (90, 86, 82, 78, 74, 70, 65, 60):
        buf = io.BytesIO()
        im.save(buf, "JPEG", quality=q, optimize=True, progressive=True)
        if buf.tell() <= max_bytes:
            break
    with open(dst, "wb") as f:
        f.write(buf.getvalue())
    return q, buf.tell()


def contact_sheet(paths, dst, labels, cols=4, max_bytes=1_500_000):
    from PIL import Image, ImageDraw
    ims = [Image.open(p).convert("RGB") for p in paths]
    w = 640
    h = round(ims[0].height * w / ims[0].width)
    rows = (len(ims) + cols - 1) // cols
    pad = 6
    sheet = Image.new("RGB", (cols * w + (cols + 1) * pad, rows * h + (rows + 1) * pad), (14, 14, 16))
    d = ImageDraw.Draw(sheet)
    for i, (im, label) in enumerate(zip(ims, labels)):
        x, y = pad + (i % cols) * (w + pad), pad + (i // cols) * (h + pad)
        sheet.paste(im.resize((w, h), Image.LANCZOS), (x, y))
        d.rectangle([x, y + h - 18, x + w, y + h], fill=(0, 0, 0))
        d.text((x + 6, y + h - 15), label, fill=(235, 235, 235))
    tmp = dst + ".png"
    sheet.save(tmp)
    q, n = jpeg(tmp, dst, max_bytes)
    os.remove(tmp)
    return n


def sequences_of(sky, project, wanted):
    """(path, player entity, length) for the wanted sequences, or every sequence a scene entity plays."""
    seqs = []
    paths = wanted or sorted(os.path.relpath(p, project) for p in glob.glob(os.path.join(project, "**", "*.sequence.json"), recursive=True))
    for p in paths:
        try:
            info = sky.call("sequence_get", sequence=p)
        except RuntimeError as e:
            if wanted:
                raise
            print(f"  skip {p}: {e}")
            continue
        player = info.get("player") or info.get("entity")
        seqs.append((p, player, float(info.get("length") or info.get("duration") or 0)))
    return seqs


def plan(seqs, frames):
    """Spreads `frames` moments over the sequences in proportion to their lengths (shot middles)."""
    total = sum(max(0.1, L) for _, _, L in seqs)
    moments = []
    for i in range(frames):
        t = (i + 0.5) / frames * total
        for path, player, L in seqs:
            L = max(0.1, L)
            if t <= L or (path, player, L) == seqs[-1]:
                moments.append((path, player, min(t, L)))
                break
            t -= L
    return moments


def main(argv=None):
    ap = argparse.ArgumentParser(description="Showcase quality gate")
    ap.add_argument("project")
    ap.add_argument("--scene", default=None)
    ap.add_argument("--sequences", default="")
    ap.add_argument("--frames", type=int, default=12)
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--samples", type=int, default=16)
    ap.add_argument("--out", default="shots")
    ap.add_argument("--stylized", action="store_true", help="flat-colored materials are a deliberate look")
    ap.add_argument("--no-strict", action="store_true", help="2%% primitive limit instead of 0.1%%")
    ap.add_argument("--perf-frames", type=int, default=30, help="real-time frames benchmarked per hero view")
    ap.add_argument("--perf-views", type=int, default=3, help="how many shots to benchmark")
    ap.add_argument("--preview", action="store_true", help="1280x720 at 8 samples (iteration)")
    a = ap.parse_args(argv)
    if a.preview:
        a.width, a.height, a.samples = 1280, 720, 8
    a.samples = min(a.samples, 16)
    project = os.path.abspath(a.project)
    scene = a.scene
    if not scene:
        cands = sorted(glob.glob(os.path.join(project, "scenes", "*.sky.json")))
        main_scene = os.path.join(project, "scenes", "main.sky.json")
        scene = os.path.relpath(main_scene if os.path.exists(main_scene) else cands[0], project)
    out = os.path.join(project, a.out)
    os.makedirs(out, exist_ok=True)
    t0 = time.time()
    sky = Sky(project=project, scene=scene)
    report = {"project": os.path.basename(project), "scene": scene, "width": a.width, "height": a.height, "samples": a.samples,
              "strict": not a.no_strict, "frames": [], "perf": [], "time": time.strftime("%Y-%m-%d %H:%M")}
    try:
        wanted = [s for s in a.sequences.split(",") if s]
        seqs = sequences_of(sky, project, wanted)
        moments = plan(seqs, a.frames) if seqs else [(None, None, 0.0)]
        shots, labels = [], []
        for i, (path, player, t) in enumerate(moments):
            png = os.path.join(out, f"shot_{i:02d}.png")
            entry = {"index": i, "sequence": path, "time": round(t, 3)}
            if path:
                sky.call("sequence_scrub", sequence=path, transient=True, time=t)
                cam = sky.call("sequence_get", sequence=path, time=t).get("camera")
                args = {"camera_entity": cam} if cam else {"view": "scene"}
            else:
                args = {"view": "scene"}
            try:
                sky.call("viewport_capture", width=a.width, height=a.height, samples=a.samples, annotate=False, overlays=False,
                         include_image=False, save_path=png, **args)
                audit_args = {"strict": not a.no_strict, "stylized": a.stylized, "width": a.width, "height": a.height}
                if path:
                    sky.call("sequence_scrub", sequence=path, clear=True)
                    audit_args.update(sequence=path, times=[t])
                elif "view" in args:
                    audit_args["view"] = args["view"]
                r = sky.call("scene_audit", **audit_args)
            finally:
                if path:
                    sky.call("sequence_scrub", sequence=path, clear=True)
            res = r["results"][0] if "results" in r else r
            entry.update(file=os.path.relpath(os.path.join(out, f"shot_{i:02d}.jpg"), project), camera=res.get("camera"),
                         pass_=res.get("pass"), primitiveCoverage=res.get("primitiveCoverage"),
                         defaultMaterialCoverage=res.get("defaultMaterialCoverage"),
                         primitiveCharacters=[c["name"] for c in res.get("primitiveCharacters", [])],
                         errors=[w["message"] for w in res.get("warnings", []) if w["severity"] == "error"][:12],
                         warnings=[w["message"] for w in res.get("warnings", []) if w["severity"] == "warning"][:12])
            entry["pass"] = entry.pop("pass_")
            q, n = jpeg(png, os.path.join(out, f"shot_{i:02d}.jpg"), 400_000)
            os.remove(png)
            entry["bytes"] = n
            shots.append(os.path.join(out, f"shot_{i:02d}.jpg"))
            labels.append(f"{i:02d}  {os.path.basename(path or scene).split('.')[0]} @ {t:.1f}s  "
                          f"{'PASS' if entry['pass'] else 'FAIL'}  prim {entry['primitiveCoverage']}%")
            report["frames"].append(entry)
            print(f"  shot {i:02d} {labels[-1]}", flush=True)
        # Performance on the first few hero views (real-time path, no supersampling).
        for e in report["frames"][: max(0, a.perf_views)]:
            cam = e.get("camera") or {}
            if not cam.get("eye"):
                continue
            p = sky.call("perf_stats", frames=a.perf_frames, width=a.width, height=a.height,
                         view={"eye": cam["eye"], "target": cam["target"], "fov": cam.get("fov", 50)})
            gpu = p.get("gpu", {})
            bench = p.get("benchmark", {})
            report["perf"].append({"shot": e["index"], "gpuMs": bench.get("gpuMsAvg", gpu.get("frameGpuMs", gpu.get("gpuMs"))),
                                   "maxGpuMs": bench.get("gpuMsMax"), "gpuFaults": gpu.get("gpuFaults", 0),
                                   "drawCalls": p.get("drawCalls"), "triangles": gpu.get("trianglesDrawn")})
        sheet = os.path.join(out, "contact_sheet.jpg")
        report["contactSheetBytes"] = contact_sheet(shots, sheet, labels) if shots else 0
    finally:
        sky.close()
    faults = sum(p.get("gpuFaults") or 0 for p in report["perf"])
    report["gpuFaults"] = faults
    report["pass"] = all(f["pass"] for f in report["frames"]) and faults == 0
    report["seconds"] = round(time.time() - t0, 1)
    with open(os.path.join(out, "gate_report.json"), "w") as f:
        json.dump(report, f, indent=2)
    failed = [f for f in report["frames"] if not f["pass"]]
    print(f"{'PASS' if report['pass'] else 'FAIL'}: {len(report['frames'])} shots, {len(failed)} failing audit, gpuFaults {faults}, "
          f"perf {[p.get('gpuMs') for p in report['perf']]} ms -> {os.path.relpath(out, os.getcwd())}/gate_report.json")
    for f in failed[:6]:
        print(f"  shot {f['index']:02d}: " + "; ".join(f["errors"][:3]))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
