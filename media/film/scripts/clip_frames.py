#!/usr/bin/env python3
"""Review helper for rendered footage: pull frames out of clips and tile them into one contact sheet.

    python3 scripts/clip_frames.py OUT.jpg CLIP.mp4 [CLIP.mp4 ...] [--at 0,0.5,1] [--w 480]

For every clip, frames at the given relative positions (0 = first frame, 1 = last) form one row; each row is
labelled with the clip name. Needs ffmpeg and ImageMagick (`magick`).
"""
import os
import shutil
import subprocess
import sys
import tempfile


def frames_of(path):
    r = subprocess.run(["ffprobe", "-v", "error", "-count_packets", "-select_streams", "v:0", "-show_entries", "stream=nb_read_packets",
                        "-of", "csv=p=0", path], capture_output=True, text=True, check=True)
    return int(r.stdout.strip() or 1)


def main():
    argv = sys.argv[1:]
    at = [float(x) for x in argv[argv.index("--at") + 1].split(",")] if "--at" in argv else [0, 0.5, 1]
    w = int(argv[argv.index("--w") + 1]) if "--w" in argv else 480
    skip = {argv.index(o) + 1 for o in ("--at", "--w") if o in argv}
    pos = [a for i, a in enumerate(argv) if not a.startswith("--") and i not in skip]
    out, clips = pos[0], pos[1:]
    tmp = tempfile.mkdtemp(prefix="clipframes-")
    tiles = []
    for ci, clip in enumerate(clips):
        n = frames_of(clip)
        name = os.path.splitext(os.path.basename(clip))[0]
        for k, a in enumerate(at):
            f = min(n - 1, max(0, round(a * (n - 1))))
            t = os.path.join(tmp, f"{ci:03d}_{k:02d}.png")
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", clip, "-vf", f"select=eq(n\\,{f}),scale={w}:-2", "-frames:v", "1", t], check=True)
            tiles.append((t, f"{name} @{f}"))
    args = ["magick", "montage"]
    for t, label in tiles:
        args += ["-label", label, t]
    args += ["-tile", f"{len(at)}x", "-geometry", "+3+3", "-pointsize", "14", "-fill", "white", "-background", "#222", out]
    subprocess.run(args, check=True)
    shutil.rmtree(tmp, ignore_errors=True)
    print(out)


if __name__ == "__main__":
    main()
