#!/usr/bin/env python3
"""Voiceover with Kokoro-82M (local TTS).  tts.py NARRATION.json OUT_DIR [voice]
Run with the Kokoro venv (needs `kokoro` and `soundfile`; keep the venv on a short path)."""
import json, os, sys
import numpy as np
import soundfile as sf
from kokoro import KPipeline

lines = json.load(open(sys.argv[1]))
out = sys.argv[2]
voice = sys.argv[3] if len(sys.argv) > 3 else "af_heart"
os.makedirs(out, exist_ok=True)
pipe = KPipeline(lang_code="a")
for line in lines:
    chunks = [audio for _, _, audio in pipe(line["text"], voice=voice, speed=1.0)]
    audio = np.concatenate([np.asarray(c) for c in chunks])
    sf.write(os.path.join(out, line["id"] + ".wav"), audio, 24000)
    print(line["id"], f"{len(audio) / 24000:.1f}s", flush=True)
