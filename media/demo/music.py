#!/usr/bin/env python3
"""Procedural ambient music bed (royalty-free by construction): warm pads + soft pluck
arpeggio + gentle pulse, with a lift at the end.   music.py OUT.wav SECONDS"""
import sys
import numpy as np
import soundfile as sf

SR = 48000
out, seconds = sys.argv[1], float(sys.argv[2])
n = int(SR * seconds)
t = np.arange(n) / SR
mix = np.zeros((n, 2))

BPM = 88.0
beat = 60.0 / BPM
bar = 4 * beat
# Fmaj9, Am7, Dm9, Bbmaj9 (MIDI notes) — calm, optimistic
chords = [[53, 57, 60, 64, 67], [57, 60, 64, 67, 72], [50, 57, 60, 64, 65], [46, 53, 57, 60, 62]]


def hz(m):
    return 440.0 * 2 ** ((m - 69) / 12)


def env(length, a, r):
    e = np.ones(length)
    # Short segments (the last chord) squeeze attack and release to fit.
    ai, ri = min(int(a * SR), length // 2), min(int(r * SR), length - length // 2)
    if ai > 0:
        e[:ai] = np.linspace(0, 1, ai) ** 2
    if ri > 0:
        e[-ri:] *= np.linspace(1, 0, ri) ** 2
    return e


# Pads: two bars per chord, detuned saw-ish via a few harmonics, slow swells.
pos = 0.0
ci = 0
while pos < seconds:
    dur = 2 * bar
    s, e_ = int(pos * SR), min(n, int((pos + dur + 1.5) * SR))
    seg_t = t[s:e_] - pos
    chord = chords[ci % len(chords)]
    pad = np.zeros((e_ - s, 2))
    for k, m in enumerate(chord):
        for det, pan in ((-0.07, 0.25), (0.07, 0.75)):
            f = hz(m) * (1 + det / 100)
            w = sum((0.55 ** h) * np.sin(2 * np.pi * f * (h + 1) * seg_t + k) for h in range(4))
            pad[:, 0] += w * (1 - pan)
            pad[:, 1] += w * pan
    pad *= env(e_ - s, 1.2, 1.6)[:, None] * 0.018
    mix[s:e_] += pad
    pos += dur
    ci += 1

# Pluck arpeggio from 6 s on (eighth notes), soft and bell-like.
arp_start = 6.0
step = beat / 2
i = 0
while arp_start + i * step < seconds - 3.0:
    p = arp_start + i * step
    chord = chords[int(p // (2 * bar)) % len(chords)]
    m = chord[[0, 2, 4, 3, 1, 2, 4, 2][i % 8]] + 12
    s = int(p * SR)
    L = int(1.2 * SR)
    seg_t = np.arange(L) / SR
    w = np.sin(2 * np.pi * hz(m) * seg_t) + 0.3 * np.sin(2 * np.pi * hz(m) * 2 * seg_t) * np.exp(-seg_t * 6)
    w *= np.exp(-seg_t * 3.2) * 0.05
    pan = 0.35 + 0.3 * ((i * 37) % 7) / 6
    e_ = min(n, s + L)
    mix[s:e_, 0] += w[: e_ - s] * (1 - pan)
    mix[s:e_, 1] += w[: e_ - s] * pan
    i += 1

# Soft sub pulse on beats 1 and 3 from 12 s (felt, not heard).
p = 12.0
while p < seconds - 4:
    s = int(p * SR)
    L = int(0.5 * SR)
    seg_t = np.arange(L) / SR
    w = np.sin(2 * np.pi * 55 * seg_t * (1 + 0.6 * np.exp(-seg_t * 20))) * np.exp(-seg_t * 9) * 0.07
    e_ = min(n, s + L)
    mix[s:e_] += w[: e_ - s, None]
    p += 2 * beat

# Gentle riser into the outro.
rs = int((seconds - 7.0) * SR)
re_ = int((seconds - 4.6) * SR)
noise = np.random.default_rng(7).standard_normal(re_ - rs)
kernel = np.ones(64) / 64
noise = np.convolve(noise, kernel, mode="same")
mix[rs:re_] += (noise * np.linspace(0, 1, re_ - rs) ** 2 * 0.06)[:, None]

# Master: fade in/out, soft limiter.
mix *= env(n, 2.0, 3.5)[:, None]
mix = np.tanh(mix * 2.2) / 2.2
mix /= max(1e-6, np.abs(mix).max()) / 0.5
sf.write(out, mix.astype(np.float32), SR)
print(f"wrote {out} ({seconds:.1f}s)")
