#!/usr/bin/env python3
"""Original procedural score for the Skywalker launch film (numpy only; no samples, no copyrighted material).

    python3 scripts/score.py [public/audio/score.wav]

Reads the film's musical grid from src/timeline.json (100 BPM, one bar = 72 frames at 30 fps), so every
section change, riser and impact lands exactly where the edit cuts. Writes a 48 kHz stereo WAV, then
normalises it with ffmpeg's two-pass loudnorm to -14 LUFS integrated, -1.5 dBTP.

Instruments: wavetable supersaw pads, a bowed-string swell for the cold open, an FM bell for the finale chord, a plucked arpeggio through a ping-pong delay,
sub bass, synthesized kick / clap / hats, noise risers, reverse swells and impacts, all into a
convolution reverb, with sidechain-style ducking from the kick.
"""
import json
import os
import subprocess
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
FILM = os.path.dirname(HERE)
TL = json.load(open(os.path.join(FILM, "src", "timeline.json")))
SR = 48000
BPM = TL["bpm"]
BEAT = 60.0 / BPM
BAR = BEAT * TL["beatsPerBar"]
N_BARS = max(s["startBar"] + s["bars"] for s in TL["sections"])
SEC_START = {s["id"]: s["startBar"] for s in TL["sections"]}
FIN = SEC_START["finale"]  # finale: triptych +0, accelerating cuts +2, hero impact +4, end card +6
TOOLS = SEC_START["tools"]
LENGTH = N_BARS * BAR + 0.5
N = int(LENGTH * SR)
rng = np.random.default_rng(20261004)
FFMPEG = os.environ.get("FFMPEG", "/opt/homebrew/bin/ffmpeg")


def section_at(bar):
    for s in TL["sections"]:
        if s["startBar"] <= bar < s["startBar"] + s["bars"]:
            return s
    return TL["sections"][-1]


def sec_id(bar):
    return section_at(bar)["id"]


def mtof(m):
    return 440.0 * 2 ** ((m - 69) / 12)


def t_of_bar(bar, beat=0.0):
    return bar * BAR + beat * BEAT


def add(buf, sig, t0, gain=1.0):
    """Mix a mono or stereo signal into a stereo buffer at time t0 (seconds)."""
    i0 = int(round(t0 * SR))
    if sig.ndim == 1:
        sig = np.stack([sig, sig], axis=1)
    if i0 < 0:
        sig = sig[-i0:]
        i0 = 0
    n = min(len(sig), len(buf) - i0)
    if n > 0:
        buf[i0:i0 + n] += sig[:n] * gain


def fft_filter(x, lo=None, hi=None, slope=1.0):
    """Zero-phase spectral band filter with soft (Butterworth-like) edges."""
    X = np.fft.rfft(x, axis=0)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    h = np.ones_like(f)
    if lo:
        h *= 1 / np.sqrt(1 + (lo / np.maximum(f, 1e-3)) ** (4 * slope))
    if hi:
        h *= 1 / np.sqrt(1 + (f / hi) ** (4 * slope))
    if X.ndim == 2:
        h = h[:, None]
    return np.fft.irfft(X * h, n=len(x), axis=0)


# ---------------------------------------------------------------- wavetables
TABLE = 4096


def saw_table(max_harm):
    ph = np.arange(TABLE) / TABLE
    out = np.zeros(TABLE)
    for k in range(1, max_harm + 1):
        out += np.sin(2 * np.pi * k * ph) / k * (1 / (1 + (k / max_harm) ** 4))
    return out / np.abs(out).max()


TABLES = {b: saw_table(b) for b in (6, 14, 30, 60)}


def osc(table, freq, n, phase0=0.0):
    ph = (phase0 + np.cumsum(np.full(n, freq / SR))) % 1.0
    return table[(ph * TABLE).astype(np.int32)]


def env_adsr(n, a, d, s, r, sustain_n=None):
    a_n, d_n, r_n = int(a * SR), int(d * SR), int(r * SR)
    sus = (sustain_n if sustain_n is not None else n) - a_n - d_n
    sus = max(0, sus)
    e = np.concatenate([
        np.linspace(0, 1, max(1, a_n)) ** 1.6,
        np.linspace(1, s, max(1, d_n)),
        np.full(sus, s),
        s * np.linspace(1, 0, max(1, r_n)) ** 2,
    ])
    if len(e) < n:
        e = np.concatenate([e, np.zeros(n - len(e))])
    return e[:n]


# ---------------------------------------------------------------- harmony
CH = {
    "D": [50, 57, 62, 66, 69, 76],
    "Dmaj9": [50, 57, 61, 64, 66, 73],
    "A": [45, 52, 57, 61, 64, 69],
    "Asus": [45, 52, 57, 62, 64, 69],
    "Bm": [47, 54, 59, 62, 66, 71],
    "Bm7": [47, 54, 57, 62, 66, 69],
    "G": [43, 50, 55, 59, 62, 67],
    "Gmaj7": [43, 50, 54, 59, 62, 66],
    "Em7": [40, 47, 55, 59, 62, 64],
    "F#m": [42, 49, 54, 57, 61, 66],
}


def chord_for(bar):
    s = sec_id(bar)
    k = bar - section_at(bar)["startBar"]
    if s == "open":
        prog = ["Dmaj9", "Bm7", "Gmaj7", "Asus"]
        if bar >= 8:
            return "D"
        return prog[(k // 2) % 4]
    if s == "problem":
        return ["Bm", "Gmaj7", "Em7", "F#m"][k % 4]
    if s in ("idea", "studio", "render", "tools"):
        return ["D", "A", "Bm", "G"][k % 4]
    if s == "wander":
        return ["Bm", "G", "D", "A"][k % 4]
    if s == "variety":
        return ["G", "D", "A", "Bm"][k % 4]
    if s == "finale":
        if bar >= FIN + 4:
            return "D"
        return ["G", "A", "Bm", "A"][k % 4]
    return "D"


def energy(bar):
    s = section_at(bar)
    e0, e1 = s["energy"]
    return e0 + (e1 - e0) * (bar - s["startBar"]) / max(1, s["bars"] - 1)


# ---------------------------------------------------------------- buses
pad = np.zeros((N, 2))
bell = np.zeros((N, 2))
arp = np.zeros((N, 2))
bass = np.zeros((N, 2))
drums = np.zeros((N, 2))
fx = np.zeros((N, 2))
kick_times = []

# ---------------------------------------------------------------- pads
for bar in range(N_BARS):
    s = sec_id(bar)
    if s == "finale" and bar >= FIN + 6:
        hold = (N_BARS - bar) * BAR + 0.4
    else:
        hold = BAR
    if s == "finale" and bar > FIN + 6:
        continue
    chord = CH[chord_for(bar)]
    e = energy(bar)
    bright = 14 if e < 0.4 else 30 if e < 0.8 else 60
    if s == "problem":
        bright = 6
    if s == "finale" and bar >= FIN + 4:
        bright = 60
    n = int((hold + 1.6) * SR)
    sig = np.zeros((n, 2))
    for j, m in enumerate(chord):
        f = mtof(m)
        for v, det in enumerate((-11, -5, 0, 6, 12)):
            fv = f * 2 ** (det / 1200)
            w = osc(TABLES[bright], fv, n, rng.random())
            pan = 0.5 + 0.38 * np.sin(v * 1.7 + j)
            sig[:, 0] += w * (1 - pan)
            sig[:, 1] += w * pan
    att = 0.9 if s in ("open", "problem") else 0.25
    en = env_adsr(n, att, 0.6, 0.85, 1.6, sustain_n=int(hold * SR))
    sig *= en[:, None] / (len(chord) * 5)
    level = 0.55 + 0.35 * e
    if s == "open" and bar < 8:
        level *= 0.55 + 0.05 * bar  # the strings carry the cold open; the pad sits underneath
    add(pad, sig, t_of_bar(bar), level)

# tame the supersaw fizz: a warm low-pass on the pad bus
pad = fft_filter(pad, lo=60, hi=6500, slope=0.7)

# ---------------------------------------------------------------- bell motif (cold open, tools, finale)
MOTIF = [(0, 74), (1.5, 78), (2, 76), (3, 81), (4, 78), (5.5, 74), (6, 76), (7, 73)]  # beats within 2 bars


def bell_note(m, dur=2.6, bright=1.0):
    n = int(dur * SR)
    t = np.arange(n) / SR
    f = mtof(m)
    idx = 2.2 * bright * np.exp(-t / 0.35)
    y = np.sin(2 * np.pi * f * t + idx * np.sin(2 * np.pi * f * 3.5 * t)) * np.exp(-t / 0.9)
    y += 0.25 * np.sin(2 * np.pi * f * 2 * t) * np.exp(-t / 0.4)
    y *= np.minimum(1, t / 0.004)
    return y


# ---------------------------------------------------------------- cold open: string swell
# Over the sketch -> clay -> final reveal the melody is played as slow, legato bowed lines
# (no struck notes): detuned saw ensembles with soft attacks, gentle vibrato and a warm
# low-pass, an airy shimmer that grows while the lines draw, and a low drone from bar 4.
strings = np.zeros((N, 2))
OPEN_LINE = [(0.0, 78, 4.0), (2.0, 76, 3.0), (3.0, 74, 3.5), (5.0, 73, 2.0), (6.0, 76, 2.2)]  # (bar, midi, bars)


def bowed(m, dur, bright=14, vib=0.0045):
    n = int((dur + 1.8) * SR)
    t = np.arange(n) / SR
    f = mtof(m)
    sig = np.zeros((n, 2))
    for v, det in enumerate((-9, -3, 4, 10)):
        ph = np.cumsum(f * 2 ** (det / 1200) * (1 + vib * np.sin(2 * np.pi * (4.6 + 0.3 * v) * t + v)) / SR)
        w = TABLES[bright][((ph + rng.random()) % 1.0 * TABLE).astype(np.int32)]
        pan = 0.5 + 0.3 * (v - 1.5) / 1.5
        sig[:, 0] += w * (1 - pan)
        sig[:, 1] += w * pan
    env = env_adsr(n, 1.4, 0.8, 0.8, 1.8, sustain_n=int(dur * SR))
    return sig * env[:, None] / 4


for bar0, m, bars in OPEN_LINE:
    add(strings, bowed(m, bars * BAR), t_of_bar(0, bar0 * 4), 0.26)
    add(strings, bowed(m - 12, bars * BAR, bright=6), t_of_bar(0, bar0 * 4), 0.12)  # cellos an octave below
strings = fft_filter(strings, lo=90, hi=3800, slope=0.8)

# airy shimmer: band-passed noise breathing in with the line drawing
n_air = int(t_of_bar(8) * SR)
air = fft_filter(rng.standard_normal((n_air, 2)), lo=4000, hi=11000)
air *= (np.linspace(0, 1, n_air) ** 1.5 * (0.6 + 0.4 * np.sin(np.arange(n_air) / SR * 2 * np.pi * 0.21)))[:, None]
add(fx, air, 0.0, 0.012)

# low drone from bar 4 into the logo hit
n_dr = int((t_of_bar(8) - t_of_bar(4) + 1.5) * SR)
td = np.arange(n_dr) / SR
drone = np.sin(2 * np.pi * mtof(38) * td) + 0.3 * np.sin(2 * np.pi * mtof(50) * td)
drone *= env_adsr(n_dr, 3.0, 0.5, 1.0, 1.5, sustain_n=n_dr - int(1.5 * SR))
add(strings, np.stack([drone, drone], 1), t_of_bar(4), 0.022)
for bar in (TOOLS, TOOLS + 2, TOOLS + 4, TOOLS + 6):
    for beat, m in MOTIF[::2]:
        add(bell, bell_note(m + 12, 2.0, 0.6), t_of_bar(bar, beat), 0.07)
for beat, m in [(0, 74), (0.02, 78), (0.04, 81), (0.06, 86)]:
    add(bell, bell_note(m, 5.0), t_of_bar(FIN + 4, beat), 0.13)
    add(bell, bell_note(m, 5.0), t_of_bar(FIN + 6, beat), 0.09)

# ---------------------------------------------------------------- arpeggio
ARP_PATTERN = [0, 2, 3, 4, 3, 2, 1, 2]


def pluck(f, dur=0.32, bright=30):
    n = int(dur * SR)
    t = np.arange(n) / SR
    w = osc(TABLES[bright], f, n) * 0.6 + np.sin(2 * np.pi * f * t) * 0.6
    return w * np.exp(-t / 0.11) * np.minimum(1, t / 0.002)


for bar in range(N_BARS):
    s = sec_id(bar)
    if s in ("open", "problem") or (s == "finale" and bar >= FIN + 4):
        continue
    if s == "wander" and bar < 42:
        continue
    chord = CH[chord_for(bar)]
    tones = [m + 12 for m in chord[1:]]
    e = energy(bar)
    sixteenth = s in ("render", "variety") or (s == "finale" and bar >= FIN + 2)
    steps = 16 if sixteenth else 8
    for k in range(steps):
        m = tones[ARP_PATTERN[k % 8] % len(tones)]
        if k >= 8 and sixteenth:
            m += 12 if k % 4 == 3 else 0
        beat = k * (4 / steps)
        acc = 1.0 if k % 4 == 0 else 0.72
        add(arp, pluck(mtof(m), bright=30 if e < 0.8 else 60), t_of_bar(bar, beat), 0.13 * acc * (0.6 + 0.5 * e))

# ping-pong dotted-eighth delay on the arp
d = int(0.75 * BEAT * SR)
dl = np.zeros_like(arp)
src = arp.copy()
for tap in range(1, 6):
    g = 0.42 ** tap
    sh = np.zeros_like(arp)
    sh[d * tap:] = src[:-d * tap]
    if tap % 2:
        dl[:, 0] += sh[:, 1] * g * 0.2
        dl[:, 1] += sh[:, 0] * g
    else:
        dl[:, 0] += sh[:, 0] * g
        dl[:, 1] += sh[:, 1] * g * 0.2
arp = fft_filter(arp, hi=9000, slope=0.7) + fft_filter(dl, lo=300, hi=6000)

# ---------------------------------------------------------------- drums


def kick(level=1.0):
    n = int(0.55 * SR)
    t = np.arange(n) / SR
    f = 46 + 110 * np.exp(-t / 0.035)
    ph = 2 * np.pi * np.cumsum(f) / SR
    y = np.sin(ph) * np.exp(-t / 0.26)
    y += 0.4 * rng.standard_normal(n) * np.exp(-t / 0.004)
    return np.tanh(1.6 * y) * level


def clap():
    n = int(0.4 * SR)
    t = np.arange(n) / SR
    noise = rng.standard_normal(n)
    e = np.zeros(n)
    for off in (0, 0.011, 0.022):
        e += np.where(t >= off, np.exp(-(t - off) / 0.012), 0)
    e += 0.6 * np.exp(-t / 0.16)
    y = fft_filter(noise * e, lo=900, hi=5200)
    return y / np.abs(y).max()


def hat(open_=False):
    n = int((0.35 if open_ else 0.09) * SR)
    t = np.arange(n) / SR
    y = fft_filter(rng.standard_normal(n), lo=7500, hi=16000) * np.exp(-t / (0.12 if open_ else 0.022))
    return y / np.abs(y).max()


KICK, CLAP, HATC, HATO = kick(), clap(), hat(), hat(True)

for bar in range(N_BARS):
    s = sec_id(bar)
    e = energy(bar)
    full = s in ("render", "variety") or (s == "finale" and FIN <= bar < FIN + 4)
    half = s in ("idea", "studio", "tools") or (s == "wander" and bar >= 44)
    if s == "idea" and bar < 22:
        half = False
    if full:
        kb = [0, 1, 2, 3]
    elif half:
        kb = [0, 2.5] if s != "tools" else [0, 2]
    else:
        kb = []
    if s == "finale" and bar >= FIN + 2:  # build: eighth-note kicks into the hit
        kb = [i * 0.5 for i in range(8)] if bar == FIN + 3 else [0, 1, 2, 3]
    for b in kb:
        t0 = t_of_bar(bar, b)
        kick_times.append(t0)
        add(drums, KICK, t0, 0.62)
    if full or (half and s in ("studio",)):
        for b in (1, 3):
            add(drums, CLAP, t_of_bar(bar, b), 0.20 if full else 0.13)
    if full or half:
        steps = 8 if not full else 16
        for k in range(steps):
            b = k * 4 / steps
            if full and k % 4 == 2:
                add(drums, HATO, t_of_bar(bar, b), 0.07)
            else:
                pan = np.array([0.8, 1.0]) if k % 2 else np.array([1.0, 0.8])
                add(drums, np.stack([HATC * pan[0], HATC * pan[1]], 1), t_of_bar(bar, b), 0.05 if k % 2 else 0.08)

# ---------------------------------------------------------------- bass
for bar in range(N_BARS):
    s = sec_id(bar)
    if s in ("open",) and bar < 8:
        continue
    if s == "finale" and bar >= FIN + 6:
        continue
    root = CH[chord_for(bar)][0]
    while root > 45:
        root -= 12
    f = mtof(root + 12) if root < 36 else mtof(root)
    e = energy(bar)
    if s in ("render", "variety") or (s == "finale" and bar < FIN + 4):
        pattern = [(i * 0.5, 0.42) for i in range(8)]
    elif s == "problem":
        pattern = [(0, 2.3)]
    else:
        pattern = [(0, 1.4), (2, 0.9), (3, 0.9)]
    for b, dur in pattern:
        n = int(dur * SR)
        t = np.arange(n) / SR
        y = np.sin(2 * np.pi * f * t) + 0.18 * np.sin(2 * np.pi * 2 * f * t)
        y = np.tanh(1.4 * y) * env_adsr(n, 0.008, 0.08, 0.8, 0.08)
        add(bass, y, t_of_bar(bar, b), 0.36 * (0.6 + 0.5 * e))

# ---------------------------------------------------------------- risers, swells, impacts


def riser(dur):
    n = int(dur * SR)
    t = np.arange(n) / SR
    hop, win = 512, 2048
    noise = rng.standard_normal(n + win)
    out = np.zeros(n + win)
    w = np.hanning(win)
    freqs = np.fft.rfftfreq(win, 1 / SR)
    for i in range(0, n, hop):
        p = i / n
        fc = 300 * (40 ** p)
        X = np.fft.rfft(noise[i:i + win] * w)
        X *= np.exp(-0.5 * ((np.log(freqs + 1) - np.log(fc)) / 0.35) ** 2)
        out[i:i + win] += np.fft.irfft(X, n=win) * w
    y = out[:n] / (np.abs(out).max() + 1e-9)
    glide = np.sin(2 * np.pi * np.cumsum(220 * 4 ** (t / dur)) / SR) * 0.25
    e = (t / dur) ** 2.2
    return (y + glide) * e


def impact():
    n = int(4.0 * SR)
    t = np.arange(n) / SR
    f = 30 + 70 * np.exp(-t / 0.08)
    boom = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t / 0.9)
    crash = fft_filter(rng.standard_normal(n), lo=2500, hi=14000) * np.exp(-t / 1.1) * 0.35
    return np.tanh(1.5 * boom) + crash


def swell(dur, lo=600, hi=9000):
    n = int(dur * SR)
    t = np.arange(n) / SR
    y = fft_filter(rng.standard_normal((n, 2)), lo=lo, hi=hi)
    y /= np.abs(y).max()
    return y * ((t / dur) ** 3)[:, None]


for r in TL["risers"]:
    t0, t1 = t_of_bar(r["fromBar"]), t_of_bar(r["toBar"])
    add(fx, riser(t1 - t0), t0, 0.22)
    add(fx, swell(1.2), t1 - 1.2, 0.18)
for h in TL["hits"]:
    t0 = t_of_bar(h["bar"])
    if h["kind"] == "impact":
        add(fx, impact(), t0, 0.55)
        kick_times.append(t0)
    else:
        add(fx, swell(0.9, 1500, 12000), t0 - 0.9, 0.08)
# soft whooshes on every section change that has no riser
riser_ends = {r["toBar"] for r in TL["risers"]}
for s in TL["sections"][1:]:
    if s["startBar"] not in riser_ends:
        add(fx, swell(0.7, 800, 7000), t_of_bar(s["startBar"]) - 0.7, 0.07)

# ---------------------------------------------------------------- UI sound design (cues from timeline.json)
FPS = TL["fps"]


def ui_click(pitch=2400.0, dur=0.05):
    n = int(dur * SR)
    t = np.arange(n) / SR
    y = fft_filter(rng.standard_normal(n), lo=pitch * 0.6, hi=pitch * 2.5) * np.exp(-t / 0.006)
    y += 0.5 * np.sin(2 * np.pi * pitch * t) * np.exp(-t / 0.01)
    return y / (np.abs(y).max() + 1e-9)


def ui_thock():
    n = int(0.25 * SR)
    t = np.arange(n) / SR
    y = np.sin(2 * np.pi * (140 + 120 * np.exp(-t / 0.02)) * t) * np.exp(-t / 0.06)
    y += 0.3 * fft_filter(rng.standard_normal(n), lo=300, hi=3000) * np.exp(-t / 0.01)
    return y / np.abs(y).max()


def ui_pop():
    n = int(0.16 * SR)
    t = np.arange(n) / SR
    f = 620 + 520 * (1 - np.exp(-t / 0.03))
    y = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t / 0.045) * np.minimum(1, t / 0.002)
    return y


sfx = TL.get("sfx", {})
for f in sfx.get("click", []):
    add(fx, ui_click(), f / FPS, 0.09)
for f in sfx.get("thock", []):
    add(fx, ui_thock(), f / FPS, 0.16)
for f in sfx.get("pop", []):
    add(fx, ui_pop(), f / FPS, 0.08)
for a, b, step in sfx.get("ticks", []):
    for k, f in enumerate(range(a, b, step)):
        add(fx, ui_click(3200 + 140 * (k % 5), 0.03), f / FPS, 0.035)
for a, b in sfx.get("typing", []):
    t = a / FPS
    while t < b / FPS:
        add(fx, ui_click(1800 + 900 * rng.random(), 0.025), t, 0.03 + 0.02 * rng.random())
        t += 0.045 + 0.05 * rng.random()

# ---------------------------------------------------------------- ducking (sidechain from the kick)
duck = np.ones(N)
tt = np.arange(int(0.35 * SR)) / SR
shape = 1 - 0.55 * np.exp(-tt / 0.09) * np.minimum(1, tt / 0.004 + 0.6)
for t0 in kick_times:
    i0 = int(t0 * SR)
    n = min(len(shape), N - i0)
    if n > 0:
        duck[i0:i0 + n] = np.minimum(duck[i0:i0 + n], shape[:n])
duck = duck[:, None]

# ---------------------------------------------------------------- reverb (FFT convolution)


def reverb_ir(seconds=3.2):
    n = int(seconds * SR)
    t = np.arange(n) / SR
    ir = rng.standard_normal((n, 2)) * np.exp(-t / 0.75)[:, None]
    ir = fft_filter(ir, lo=180, hi=7000)
    ir[: int(0.012 * SR)] = 0  # pre-delay
    return ir / np.sqrt((ir ** 2).sum(axis=0))


def convolve(x, ir, block=1 << 16):
    m = len(ir)
    nfft = 1 << int(np.ceil(np.log2(block + m - 1)))
    H = np.fft.rfft(ir, nfft, axis=0)
    out = np.zeros((len(x) + m, 2))
    for i in range(0, len(x), block):
        seg = x[i:i + block]
        Y = np.fft.irfft(np.fft.rfft(seg, nfft, axis=0) * H, nfft, axis=0)
        n = min(len(Y), len(out) - i)
        out[i:i + n] += Y[:n]
    return out[: len(x)]


send = pad * 0.55 + bell * 1.0 + strings * 0.8 + arp * 0.6 + drums * 0.08 + fx * 0.5
wet = convolve(send, reverb_ir())

mix = (
    pad * duck * 0.9
    + bell * 0.9
    + strings * 0.85
    + arp * duck * 0.8
    + bass * duck * 0.95
    + drums * 0.9
    + fx * 0.85
    + wet * 0.42
)
# master: gentle high-pass, glue saturation, fade tail
mix = fft_filter(mix, lo=28, hi=17000)
peak = np.abs(mix).max()
mix = np.tanh(1.2 * mix / peak) / np.tanh(1.2)
tail = int(1.2 * SR)
mix[-tail:] *= np.linspace(1, 0, tail)[:, None] ** 2
mix *= 0.8


def write_wav(path, x):
    x16 = (np.clip(x, -1, 1) * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(x16.tobytes())


def loudnorm(src, dst):
    probe = subprocess.run([FFMPEG, "-hide_banner", "-nostats", "-i", src, "-af", "loudnorm=I=-14:TP=-1.5:LRA=11:print_format=json", "-f", "null", "-"],
                           capture_output=True, text=True).stderr
    j = json.loads(probe[probe.rindex("{"):probe.rindex("}") + 1])
    af = ("loudnorm=I=-14:TP=-1.5:LRA=11:linear=true:"
          f"measured_I={j['input_i']}:measured_TP={j['input_tp']}:measured_LRA={j['input_lra']}:measured_thresh={j['input_thresh']}:offset={j['target_offset']}")
    subprocess.run([FFMPEG, "-y", "-hide_banner", "-loglevel", "error", "-i", src, "-af", af, "-ar", str(SR), "-c:a", "pcm_s16le", dst], check=True)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(FILM, "public", "audio", "score.wav")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    raw = out + ".raw.wav"
    write_wav(raw, mix)
    loudnorm(raw, out)
    os.remove(raw)
    print(f"wrote {out}: {LENGTH:.1f}s, {N_BARS} bars at {BPM} BPM")
