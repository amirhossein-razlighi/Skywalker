#!/usr/bin/env python3
"""Contact sheets for review: python3 scripts/sheets.py KEYFRAME_DIR OUT_PREFIX [per_sheet=24] [cols=4]"""
import os
import subprocess
import sys

src, prefix = sys.argv[1], sys.argv[2]
per = int(sys.argv[3]) if len(sys.argv) > 3 else 24
cols = int(sys.argv[4]) if len(sys.argv) > 4 else 4
files = sorted(f for f in os.listdir(src) if f.endswith(".jpg"))
for k in range(0, len(files), per):
    chunk = [os.path.join(src, f) for f in files[k:k + per]]
    out = f"{prefix}_{k // per:02d}.jpg"
    subprocess.run(["magick", "montage", *chunk, "-tile", f"{cols}x", "-geometry", "480x270+3+3", "-pointsize", "15",
                    "-label", "%t", "-fill", "white", "-background", "#222", out], check=True)
    print(out)
