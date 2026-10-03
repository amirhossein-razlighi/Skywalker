# Audio

Skywalker plays sound with [miniaudio](https://miniaud.io) (public domain / MIT-0) and, because
an agent cannot download a sound pack or listen to what it made, it also **generates** sound:
a synthesizer for effects and ambience, a music composer, and tools to measure the result.

- [Quick start](#quick-start)
- [Components](#components)
- [Mixer and buses](#mixer-and-buses)
- [Wander](#wander)
- [Generating sound](#generating-sound)
- [Generating music](#generating-music)
- [Checking audio without ears](#checking-audio-without-ears)
- [Headless, CI and determinism](#headless-ci-and-determinism)
- [Agent recipes](#agent-recipes)
- [Reference](#reference)

## Quick start

```
audio_generate {"preset": "footstep_stone", "variations": 4, "path": "audio/step.wav"}
audio_generate {"preset": "fire_crackle_loop", "path": "audio/fire.wav", "entity": "Campfire"}
audio_generate {"music": {"mood": "adventure", "bars": 16}, "path": "audio/theme.wav"}
```

```wander
behavior Player
  on key "space"
    play_sound("audio/step_1.wav", 0.8)
  end
  on start
    music("audio/theme.wav", 2)        -- crossfade in over 2 seconds
  end
end
```

Sound plays only while the game plays. Press play (or `sim_control play`) to hear it; stop
silences everything.

## Components

**`audio`** (`AudioSource`) plays a clip from the entity's position.

| Field | Default | Meaning |
|---|---|---|
| `clip` | `""` | Project path to a wav, mp3 or flac file. |
| `volume` | `1` | Linear gain, 0..4. |
| `pitch` | `1` | Playback speed multiplier, 0.1..4 (also shifts pitch). |
| `loop` | `false` | Repeat forever (ambience, music, engines). |
| `playOnStart` | `true` | Start when the game starts. Otherwise start it with `play(e)`. |
| `spatial` | `true` | 3D sound: louder when close, panned left/right. `false` = plain stereo (music, UI). |
| `minDistance` | `1` | Full volume inside this distance (m). |
| `maxDistance` | `50` | Distance where attenuation stops (`inverse`, `exponential`) or reaches silence (`linear`). |
| `rolloff` | `inverse` | Falloff curve: `inverse` (natural), `linear`, `exponential`, `none`. |
| `rolloffFactor` | `1` | Steepness of the falloff. |
| `bus` | `sfx` | Mixer bus: `master`, `music`, `sfx`, `ambience`, `voice`, `ui`. |
| `doppler` | `0` | Doppler effect for moving emitters: 0 off, 1 realistic. |

**`listener`** (`AudioListener`) is the pair of ears. It is optional: without one, sound is heard
from the active scene camera. Put it on the player character to hear from there. `volume`
scales everything that listener hears.

Distance gain follows the OpenAL models the mixer uses (`d` clamped to `[min, max]`):

```
inverse      min / (min + rolloffFactor * (d - min))
linear       1 - rolloffFactor * (d - min) / (max - min)
exponential  (d / min) ^ -rolloffFactor
```

At `rolloffFactor` 1, `inverse` gives 1/2 at twice the minimum distance and 1/10 at ten times.
Only `linear` reaches silence. `mix`ed voices report their current `distanceGain` in `audio_mix`.

Components are reflected like every other: `entity_update`, `entity_create`, scene files,
undo, and `self.audio.volume = 0.5` in Wander all work.

## Mixer and buses

Every sound plays through a bus; buses feed `master`.

| Bus | Use |
|---|---|
| `master` | Everything. |
| `music` | Background music (`music()` always uses it). |
| `sfx` | Effects, `play_sound` one-shots (default). |
| `ambience` | Wind, rain, fire, room tone. |
| `voice` | Dialogue and narration. |
| `ui` | Menu clicks. Keeps playing while the game is paused. |

The project mix is stored in **`audio.json`** at the project root and edited with `audio_mix`
(or by hand; changes are picked up within two seconds):

```json
{ "buses": { "music": {"volume": 0.5, "mute": false}, "ambience": {"volume": 0.8, "mute": false} },
  "musicFade": 1.5 }
```

`set_volume("music", 0.3)` in Wander is a *runtime* override (a cutscene, a pause menu). It
ends when the game stops and the project mix returns. `mute` is a project setting only.

`music("audio/new.wav", 2)` crossfades: the new track fades in over 2 s while the old one fades
out and is released. Asking for the track that is already playing does nothing. `music("")`
fades the music out. Music is streamed from disk; effects are decoded up front.

## Wander

```
play(e)                    start entity e's audio component (restarts it)
stop_sound(e)              silence e
play_sound("path", vol?)   one-shot at self's position (3D, sfx bus); vol defaults to 1
music("path", fade?)       crossfade the music; "" fades out; fade defaults to the mix's musicFade
set_volume("bus", v)       runtime bus volume (0..4)
```

A missing or undecodable file is a script runtime error naming the file (and, like any runtime
error, five of them disable the script). At most 48 one-shots play at once; the oldest is
dropped. Components can also be driven by property: `self.audio.pitch = 1 + speed * 0.05`.

## Generating sound

`audio_generate` writes a 16-bit WAV into the project, registers it as an asset (type `audio`,
tagged `generated`) and stores its **provenance** in the `.meta` file: generator, preset, seed,
and the full parameter set, so the sound can be regenerated or tweaked exactly. Output is
deterministic: the same preset, seed and parameters give byte-identical files.

### Presets

| Preset | What it is |
|---|---|
| `jump`, `coin`, `pickup`, `powerup`, `error` | Chiptune-style feedback sounds. |
| `hit`, `explosion`, `laser`, `shoot`, `swoosh` | Impacts, blasts and weapon sounds. |
| `footstep_grass`, `footstep_stone`, `footstep_wood` | Footsteps; use `variations` or change `seed` for each step. |
| `door` | Creak then thud. |
| `ui_click`, `ui_hover` | Interface sounds. |
| `wind_loop`, `rain_loop`, `fire_crackle_loop`, `ocean_waves_loop` | Seamless ambience (10 to 16 s). |
| `ambient_drone` | Seamless dark drone pad (20 s). |
| `heartbeat` | One lub-dub at 70 bpm; loops cleanly. |

`audio_generate {"list": true}` lists presets with descriptions, the music moods, and the
complete parameter reference.

Useful arguments: `seed`, `variations` (1 to 16, files get `_1`, `_2`...), `pitch` (multiplier
on tonal layers), `volume`, `entity` (create or update that entity's `audio` component with the
right `loop`, `bus` and `spatial` for the kind of sound), `description`, `tags`.

### Custom sounds

A sound is a stack of **layers** run through an optional echo and reverb. Pass `params`
(overrides on top of a preset, or a complete sound without one):

```json
{ "params": {
    "layers": [
      { "wave": "saw",  "freq": 1800, "slide": -3.2, "minFreq": 140,
        "attack": 0.001, "decay": 0.2, "lowpass": 7000, "lowpassEnd": 1200, "resonance": 0.4 },
      { "wave": "noise", "highpass": 3000, "decay": 0.04, "gain": 0.3 } ],
    "echo": {"time": 0.07, "feedback": 0.3, "mix": 0.18},
    "reverb": {"mix": 0.15, "room": 0.4} },
  "path": "audio/zap.wav" }
```

A single layer can be written flat: `{"params": {"wave": "sine", "freq": 880, "decay": 0.3}}`.

| Group | Fields |
|---|---|
| Source | `wave`: `sine square saw triangle noise pink brown fm grains`. `fm` is a phase-modulated sine (`fmRatio`, `fmDepth`: bells, metallic hits); `grains` are random decaying bursts (`grainRate`, `grainDecay`, `grainSpread`: rain, crackle, gravel). |
| Pitch | `freq` (Hz), `slide` (octaves/s), `slideAccel`, `minFreq` (layer ends below it), `arp` (semitone offsets), `arpRate`, `arpLoop`, `vibratoDepth`, `vibratoRate`. |
| Square | `duty`, `dutySweep`. |
| Envelope | `attack`, `decay`, `sustain` (level), `hold`, `release`, `punch`. |
| Level | `gain`, `tremolo`, `tremoloRate`, `ampWander` (slow random loudness drift). |
| Filter | `lowpass`, `lowpassEnd`, `highpass`, `highpassEnd` (sweeps), `resonance`, `filterWander`, `wanderRate`. |
| Shape | `drive`, `bitcrush`, `downsample`. |
| Timing | `delay` (start offset: build multi-part sounds from several layers). |
| Global | `echo {time, feedback, mix}`, `reverb {mix, room, damping}`, `volume`, `normalize` (peak, default 0.9), `duration`, `loopSeconds`, `sampleRate`, `seed`. |

`loopSeconds` renders a **seamless loop** of exactly that length: the signal that follows the
loop point is crossfaded into the start, so the wrap continues the waveform. Every `*_loop`
preset uses it. Mistakes get hints: `"freqq"` answers *did you mean 'freq'?*, values outside
a field's range name the allowed range.

Levels are managed for you: sounds are peak-normalized and their average level is capped near
-15 dBFS, and a DC blocker, a few milliseconds of fade at the end and trimming of the inaudible
reverb tail keep files clean. Use `volume` to place a sound in the mix.

## Generating music

`audio_generate {"music": {...}}` composes a loop: a chord progression played by a pad, bass,
an arpeggio and a motif-based melody over drums, mixed in stereo with reverb and levelled to
about -18 LUFS. Everything is seeded.

| Field | Meaning |
|---|---|
| `mood` | `calm`, `happy`, `sad`, `tense`, `epic`, `mysterious`, `eerie`, `adventure`, `battle`. Sets mode, tempo, progression pool, instrumentation and drum style. |
| `tempo` | Beats per minute (30 to 240). Default from the mood. |
| `key` | `"A minor"`, `"C major"`, `"F# dorian"`. Modes: major, minor, dorian, phrygian, lydian, mixolydian, locrian, harmonic_minor. |
| `progression` | Roman numerals, one chord per bar: `"i VI III VII"`. Uppercase = major triad, lowercase = minor. Plain numerals are diatonic to the mode; `b`/`#` numerals are measured from the major scale (`bVII`, `bII`). Suffixes: `7`, `maj7`, `dim`, `sus4`, `sus2`, `add9`. |
| `bars` | Length in 4/4 bars, 2 to 64 (the loop is exactly this long). |
| `energy` | 0..1: busier arpeggio, denser melody and drums. |
| `pad`, `bass`, `arp`, `melody`, `drums` | Turn instruments off. |
| `reverb` | 0..1. |
| `seed` | Another seed is another piece (melody, rhythm and, without `progression`, chords). |

Tips: give each game two or three moods and let `music()` crossfade between them (calm
exploration, tense approach, battle). For a quiet layer under dialogue, generate with
`drums:false, melody:false` and a lower bus volume.

## Checking audio without ears

Agents cannot listen, so `audio_info` measures a file:

- duration, sample rate, channels;
- peak and RMS in dBFS, **integrated loudness in LUFS** (ITU-R BS.1770);
- clipped samples, DC offset, silence ratio;
- first/last sample (a non-zero start or end clicks);
- for loops: the jump at the loop point compared with the signal's normal motion (`seamless`).

It returns **warnings** in plain words (clipping, too loud or quiet, DC offset, clicks, loop
jump) and the asset's provenance. Rough targets: sound effects -23 to -14 LUFS, music -20 to
-16, ambience a bit lower. `audio_mix` shows what is playing (clip, bus, distance gain per
voice) and, without an output device, the measured output peak. `audio_play` auditions a clip
(edit mode) or starts a sound (play mode) and says whether a real device is used.

## Headless, CI and determinism

- `SKYWALKER_AUDIO=auto|null|off` (and `EngineConfig::audio`) selects the mode. **auto** opens
  the output device lazily on the first sound and falls back to null output when there is none;
  **null** never opens a device but still mixes in simulated time; **off** disables audio and
  makes every call a silent no-op. The engine always starts, with or without a device.
- In null mode, playback state, voices, spatialization and levels are real and testable:
  `AudioSystem::mixdown` returns what would have been played, which is how the test suite checks
  panning and distance attenuation.
- Generation is a pure function of its parameters. Sound *playback* is not part of the
  deterministic simulation, so audio never changes gameplay state, and replays are identical
  with or without a device.
- Pausing the game pauses all sound except the `ui` bus; stop discards every voice.

## Agent recipes

**A footstep that varies.** Generate four variations and pick one in Wander:

```
audio_generate {"preset":"footstep_grass","variations":4,"path":"audio/step.wav"}
```
```wander
behavior Footsteps
  var distanceWalked = 0
  var last = vec(0, 0, 0)
  on start
    last = self.position
  end
  on tick
    distanceWalked = distanceWalked + distance(self.position, last)
    last = self.position
    if distanceWalked > 1.6 then
      distanceWalked = 0
      play_sound("audio/step_" + str(floor(random(1, 5))) + ".wav", 0.6)
    end
  end
end
```

**Ambience that follows the world.** A looping campfire that is quiet beyond 25 m:

```
audio_generate {"preset":"fire_crackle_loop","entity":"Campfire","path":"audio/fire.wav"}
entity_update {"entity":"Campfire","components":{"audio":{"maxDistance":25,"rolloff":"linear","volume":0.8}}}
```

**Music that reacts.** Calm until an enemy is near:

```wander
behavior MusicDirector
  var mode = "calm"
  on tick
    let near = nearest("enemy")
    if exists(near) and distance(self, near) < 15 then
      if mode != "battle" then
        mode = "battle"
        music("audio/battle.wav", 1)
      end
    elif mode != "calm" then
      mode = "calm"
      music("audio/calm.wav", 3)
    end
  end
end
```

**Mix the game.** Music under effects, ambience a little lower, then check the result:

```
audio_mix {"buses": {"music": 0.55, "ambience": 0.7, "sfx": 1}}
audio_info {"path": "audio/theme.wav"}
```

**Pause menu.** `set_volume("music", 0.25)` on pause and back on resume. The `ui` bus keeps
playing, so menu clicks work while everything else is frozen.

**Debug "why is it silent?"** `audio_mix` (are there voices? is the bus muted? is a device in
use or only the null output?), then `audio_info` on the clip (silent file?), then check the
distance gain of the voice (`distanceGain` near 0 means the listener is far: raise
`maxDistance` or use `rolloff: "inverse"`).

## Reference

Tools: `audio_generate`, `audio_play`, `audio_mix`, `audio_info`.
Wander: `play`, `play_sound`, `stop_sound`, `music`, `set_volume`.
Components: `audio`, `listener`.
Files: `audio.json` (mix), generated `*.wav` plus `.meta` provenance.
Code: `engine/src/audio/` (`Synth`, `SfxPresets`, `MusicGen`, `Wav`, `AudioSystem`),
`engine/src/agent/AudioTools.cpp`.
Third-party: miniaudio 0.11.22 (public domain or MIT-0), fetched at configure time.
