# Audio

Skywalker plays positional sound, music and interface sounds through a mixer with six buses, using [miniaudio](https://miniaud.io) for output. Because an agent cannot download a sound pack or listen to what it made, the engine also **generates** sound effects, ambience loops and music deterministically, and **measures** audio (loudness, clipping, clicks, loop seams) so the result can be checked without ears.

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/audio-details.webp · The Details panel of a campfire entity with its audio component: clip, loop, bus ambience, spatial, min and max distance, rolloff</div>

## Concepts

### The audio component

An `audio` component plays a clip from the entity's position.

| Field | Default | Meaning |
|---|---|---|
| `clip` | `""` | Project path to a WAV, MP3 or FLAC file |
| `volume` | 1 | Linear gain, 0–4 |
| `pitch` | 1 | Playback speed multiplier, 0.1–4 (also shifts pitch) |
| `loop` | false | Repeat forever: ambience, music, engines |
| `playOnStart` | true | Start when the game starts; otherwise start it with `play(e)` |
| `spatial` | true | 3D sound: louder when close, panned left and right. `false` = plain stereo (music, UI) |
| `minDistance` | 1 | Full volume inside this distance (m) |
| `maxDistance` | 50 | Distance where attenuation stops (`inverse`, `exponential`) or reaches silence (`linear`) |
| `rolloff` | `inverse` | Falloff curve: `inverse` (natural), `linear`, `exponential`, `none` |
| `rolloffFactor` | 1 | Steepness of the falloff |
| `bus` | `sfx` | Mixer bus: `master`, `music`, `sfx`, `ambience`, `voice`, `ui` |
| `doppler` | 0 | Doppler effect for moving emitters: 0 off, 1 realistic |

Sound plays only while the game plays. Stopping the game silences everything.

### The listener

The `listener` component (`enabled`, `volume`) is the pair of ears. It is optional: without one, sound is heard from the active scene camera. Put it on the player character to hear from there; `volume` scales everything that listener hears.

### Distance models

Distance gain uses these models, with the distance `d` clamped to [`minDistance`, `maxDistance`]:

| `rolloff` | Gain |
|---|---|
| `inverse` | min / (min + rolloffFactor × (d − min)) |
| `linear` | 1 − rolloffFactor × (d − min) / (max − min) |
| `exponential` | (d / min) ^ −rolloffFactor |
| `none` | 1 |

At `rolloffFactor` 1, `inverse` gives 1/2 at twice the minimum distance and 1/10 at ten times. Only `linear` reaches silence. `audio_mix` reports the current `distanceGain` of every playing voice.

### Mixer and buses

Every sound plays through a bus, and every bus feeds `master`.

| Bus | Use |
|---|---|
| `master` | Everything |
| `music` | Background music; `music()` always uses it |
| `sfx` | Effects and `play_sound` one-shots (the default) |
| `ambience` | Wind, rain, fire, room tone |
| `voice` | Dialogue and narration |
| `ui` | Menu clicks; keeps playing while the game is paused |

The project mix lives in `audio.json` at the project root. Edit it with `audio_mix` or by hand; changes are picked up within two seconds:

```json
{
  "buses": {"music": {"volume": 0.5, "mute": false}, "ambience": {"volume": 0.8, "mute": false}},
  "musicFade": 1.5
}
```

`set_volume("music", 0.3)` in Wander is a runtime override (a cutscene, a pause menu): it ends when the game stops and the project mix returns. `mute` is a project setting only.

`music("audio/new.wav", 2)` crossfades: the new track fades in over 2 s while the old one fades out. Asking for the track that is already playing does nothing; `music("")` fades the music out. Music streams from disk; effects are decoded up front.

## How to play sound

=== "Wander"

    ```wander
    behavior Player
      intent "Start the theme when the game starts; a step sound on space."
      on start
        music("audio/theme.wav", 2)        -- crossfade in over 2 seconds
      end
      on key "space"
        play_sound("audio/step_1.wav", 0.8)
      end
    end
    ```

=== "Component"

    ```tool
    entity_update {"entity": "Campfire", "components": {"audio": {"clip": "audio/fire.wav", "loop": true, "bus": "ambience", "maxDistance": 25, "rolloff": "linear", "volume": 0.8}}}
    ```

=== "Tool call"

    ```tool
    audio_play {"clip": "audio/jump.wav"}
    audio_play {"entity": "Campfire"}
    ```

| Builtin | Does |
|---|---|
| `play(e)` | Starts (or restarts) entity `e`'s audio component |
| `stop_sound(e)` | Silences `e` |
| `play_sound(path, volume)` | A one-shot at the behavior's entity (3D, `sfx` bus); volume defaults to 1 |
| `music(path, fade)` | Crossfades the music; `""` fades out; fade defaults to the mix's `musicFade` |
| `set_volume(bus, v)` | Runtime bus volume (0–4) |

Components are properties too: `self.audio.pitch = 1 + speed * 0.05`. A missing or undecodable file is a runtime error naming the file. At most 48 one-shots play at once; the oldest is dropped.

`audio_play` auditions a clip while editing (nothing from the scene plays in edit mode) or starts a sound while playing, and reports whether a real output device is used.

## How to generate sound

`audio_generate` writes a 16-bit WAV into the project, registers it as an `audio` asset tagged `generated`, and stores its provenance in the `.meta` file: generator, preset, seed and the full parameter set, so the sound can be regenerated or tweaked exactly. Output is deterministic: the same preset, seed and parameters give byte-identical files.

```tool
audio_generate {"preset": "footstep_stone", "variations": 4, "path": "audio/step.wav"}
audio_generate {"preset": "fire_crackle_loop", "path": "audio/fire.wav", "entity": "Campfire"}
audio_generate {"preset": "laser", "pitch": 0.7, "seed": 3, "path": "audio/blaster.wav"}
audio_generate {"list": true}
```

### Presets

| Preset | What it is |
|---|---|
| `jump`, `coin`, `pickup`, `powerup`, `error` | Chiptune-style feedback sounds |
| `hit`, `explosion`, `laser`, `shoot`, `swoosh` | Impacts, blasts and weapons |
| `footstep_grass`, `footstep_stone`, `footstep_wood` | Footsteps; use `variations` or change `seed` for each step |
| `door` | A creak, then a thud |
| `ui_click`, `ui_hover` | Interface sounds |
| `wind_loop`, `rain_loop`, `fire_crackle_loop`, `ocean_waves_loop` | Seamless ambience (10–16 s) |
| `ambient_drone` | A seamless dark drone pad (20 s) |
| `heartbeat` | One lub-dub at 70 bpm; loops cleanly |

Useful arguments: `seed`, `variations` (1–16; files get `_1`, `_2`... suffixes), `pitch` (multiplier on tonal layers), `volume`, `entity` (creates or updates that entity's `audio` component with a suitable `loop`, `bus` and `spatial` for the kind of sound), `description` and `tags`.

### Custom sounds

A sound is a stack of **layers** run through an optional echo and reverb. Pass `params`, either as overrides on top of a preset or as a complete sound:

```tool
audio_generate {"path": "audio/zap.wav", "params": {"layers": [{"wave": "saw", "freq": 1800, "slide": -3.2, "minFreq": 140, "attack": 0.001, "decay": 0.2, "lowpass": 7000, "lowpassEnd": 1200, "resonance": 0.4}, {"wave": "noise", "highpass": 3000, "decay": 0.04, "gain": 0.3}], "echo": {"time": 0.07, "feedback": 0.3, "mix": 0.18}, "reverb": {"mix": 0.15, "room": 0.4}}}
audio_generate {"path": "audio/bell.wav", "params": {"wave": "fm", "freq": 660, "fmRatio": 3.5, "fmDepth": 2, "decay": 1.4, "reverb": {"mix": 0.3, "room": 0.7}}}
```

A single layer can be written flat, as in the bell above.

| Group | Fields |
|---|---|
| Source | `wave`: `sine`, `square`, `saw`, `triangle`, `noise`, `pink`, `brown`, `fm` (a phase-modulated sine: `fmRatio`, `fmDepth`; bells, metallic hits), `grains` (random decaying bursts: `grainRate`, `grainDecay`, `grainSpread`; rain, crackle, gravel) |
| Pitch | `freq` (Hz), `slide` (octaves/s), `slideAccel`, `minFreq` (the layer ends below it), `arp` (semitone offsets), `arpRate`, `arpLoop`, `vibratoDepth`, `vibratoRate` |
| Square | `duty`, `dutySweep` |
| Envelope | `attack`, `decay`, `sustain` (level), `hold`, `release`, `punch` |
| Level | `gain`, `tremolo`, `tremoloRate`, `ampWander` (slow random loudness drift) |
| Filter | `lowpass`, `lowpassEnd`, `highpass`, `highpassEnd` (sweeps), `resonance`, `filterWander`, `wanderRate` |
| Shape | `drive`, `bitcrush`, `downsample` |
| Timing | `delay` (start offset: build multi-part sounds from several layers) |
| Global | `echo {time, feedback, mix}`, `reverb {mix, room, damping}`, `volume`, `normalize` (peak, default 0.9), `duration`, `loopSeconds`, `sampleRate`, `seed` |

`loopSeconds` renders a **seamless loop** of exactly that length: the signal after the loop point is crossfaded into the start, so the wrap continues the waveform. Every `*_loop` preset uses it. Mistyped fields get did-you-mean hints, and values out of range name the allowed range.

Levels are managed for you: sounds are peak-normalized, their average level is capped near −15 dBFS, and a DC blocker, a few milliseconds of fade at the end and trimming of the inaudible reverb tail keep files clean. Use `volume` to place a sound in the mix.

## How to generate music

`audio_generate` with `music` composes a loop: a chord progression played by a pad, bass, an arpeggio and a motif-based melody over drums, mixed in stereo with reverb and levelled to about −18 LUFS. Everything is seeded.

```tool
audio_generate {"music": {"mood": "adventure", "bars": 16}, "path": "audio/theme.wav"}
audio_generate {"music": {"mood": "tense", "key": "D minor", "tempo": 96, "progression": "i VI III VII", "bars": 8, "energy": 0.6}, "path": "audio/approach.wav"}
audio_generate {"music": {"mood": "calm", "drums": false, "melody": false, "bars": 8}, "path": "audio/dialogue_bed.wav"}
```

| Field | Meaning |
|---|---|
| `mood` | `calm`, `happy`, `sad`, `tense`, `epic`, `mysterious`, `eerie`, `adventure`, `battle`: sets mode, tempo, progression pool, instrumentation and drum style |
| `tempo` | Beats per minute, 30–240; default from the mood |
| `key` | `"A minor"`, `"C major"`, `"F# dorian"`. Modes: major, minor, dorian, phrygian, lydian, mixolydian, locrian, harmonic_minor |
| `progression` | Roman numerals, one chord per bar: `"i VI III VII"`. Uppercase = major triad, lowercase = minor. Plain numerals are diatonic to the mode; `b`/`#` numerals are measured from the major scale (`bVII`, `bII`). Suffixes: `7`, `maj7`, `dim`, `sus4`, `sus2`, `add9` |
| `bars` | Length in 4/4 bars, 2–64; the loop is exactly this long |
| `energy` | 0–1: busier arpeggio, denser melody and drums |
| `pad`, `bass`, `arp`, `melody`, `drums` | Turn instruments off |
| `reverb` | 0–1 |
| `seed` | Another seed is another piece: melody, rhythm and, without `progression`, chords |

Give each game two or three moods and let `music()` crossfade between them: calm exploration, a tense approach, battle. For a quiet bed under dialogue, generate with `drums` and `melody` off and a lower bus volume.

## How to check audio without ears

```tool
audio_info {"path": "audio/theme.wav"}
audio_mix {}
```

`audio_info` measures a file: duration, sample rate, channels; peak and RMS in dBFS; integrated loudness in LUFS (ITU-R BS.1770); clipped samples, DC offset and silence ratio; the first and last sample (a non-zero start or end clicks); and, for loops, the jump at the loop point compared with the signal's normal motion (`seamless`). It returns warnings in plain words and the asset's provenance.

Rough loudness targets: sound effects −23 to −14 LUFS, music −20 to −16 LUFS, ambience a little lower.

`audio_mix` with no arguments returns the mix plus what is playing right now (each voice with its clip, bus and distance gain), whether a real output device is used, and, without a device, the measured output peak.

## Headless mode and determinism

- The environment variable `SKYWALKER_AUDIO` selects the mode: `auto` (default) opens the output device on the first sound and falls back to null output when there is none; `null` never opens a device but still mixes in simulated time; `off` disables audio and makes every call a silent no-op. The engine always starts, with or without a device.
- In null mode, playback state, voices, spatialization and levels are real and testable, which is how the test suite checks panning and distance attenuation on machines without sound hardware.
- Generation is a pure function of its parameters. Playback is not part of the deterministic simulation: audio never changes gameplay state, and replays are identical with or without a device.
- Pausing the game pauses every sound except the `ui` bus, and resuming continues held voices where they stopped. Stopping the game discards every voice. The editor's own pause holds every bus.

## Recipes

### Footsteps that vary

```tool
audio_generate {"preset": "footstep_grass", "variations": 4, "path": "audio/step.wav"}
```

```wander
behavior Footsteps
  intent "Play a random footstep every 1.6 m walked."
  param stride = 1.6 in 0.5..3 "meters between steps"
  var walked = 0
  var last = vec(0, 0, 0)

  on start
    last = self.position
  end

  on tick
    walked += distance(self.position, last)
    last = self.position
    if walked > stride then
      walked = 0
      play_sound("audio/step_" + str(random_int(1, 4)) + ".wav", 0.6)
    end
  end
end
```

### Ambience that follows the world

A looping campfire that fades to silence at 25 m:

```tool
audio_generate {"preset": "fire_crackle_loop", "entity": "Campfire", "path": "audio/fire.wav"}
entity_update {"entity": "Campfire", "components": {"audio": {"maxDistance": 25, "rolloff": "linear", "volume": 0.8}}}
```

### Music that reacts

```wander
behavior MusicDirector
  intent "Calm music while exploring; battle music when an enemy is within 15 m."
  var mode = "calm"
  on tick
    let near = nearest("enemy", 15)
    if exists(near) then
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

### Duck the music under a pause menu

Put this on an entity whose `process` mode is `always`, so it keeps running while the game is paused:

```wander
behavior PauseDucking
  intent "Lower the music while the game is paused and restore it on resume."
  on pause
    set_volume("music", 0.25)
  end
  on resume
    set_volume("music", 1)
  end
end
```

### Mix the game

```tool
audio_mix {"buses": {"music": 0.55, "ambience": 0.7, "sfx": 1}}
audio_mix {"buses": {"ambience": {"volume": 0.7, "mute": true}}}
```

### Why is it silent?

1. `audio_mix`: are there voices, is the bus muted, is a device in use or only the null output?
2. `audio_info` on the clip: is the file silent?
3. The voice's `distanceGain`: near 0 means the listener is far away; raise `maxDistance` or use `rolloff` `inverse`.

## Pitfalls

- **Nothing plays while editing.** Scene sound plays only while the game runs; use `audio_play` to audition a clip.
- **One-shot limit.** At most 48 `play_sound` one-shots play at once; the oldest is dropped.
- **Missing files are errors.** A missing or undecodable clip raises a runtime error; five runtime errors disable a script.
- **Runtime volume is temporary.** `set_volume` ends when the game stops; set the project mix with `audio_mix`.
- **Linear rolloff reaches silence; the others do not.** Use `linear` when a sound must be inaudible beyond `maxDistance`.

!!! agent "For agents"

    You cannot hear: generate deterministically, then measure.

    ```tool
    audio_generate {"preset": "coin", "path": "audio/coin.wav"}                   # deterministic WAV with provenance
    audio_info {"path": "audio/coin.wav"}                                          # LUFS, peak, clicks, loop seam
    audio_generate {"music": {"mood": "calm", "bars": 16}, "path": "audio/calm.wav"}
    audio_mix {"buses": {"music": 0.5}}                                            # balance the buses
    audio_mix {}                                                                   # voices and distance gains while playing
    ```

    Keep effects between −23 and −14 LUFS and music between −20 and −16 LUFS, and fix every warning `audio_info` reports.

## Reference

- Tools: [`audio_generate`](../reference/tools/asset.md#audio_generate), [`audio_info`](../reference/tools/asset.md#audio_info), [`audio_play`](../reference/tools/sim.md#audio_play), [`audio_mix`](../reference/tools/sim.md#audio_mix).
- Components: [`audio`](../reference/components/audio.md#audio), [`listener`](../reference/components/audio.md#listener).
- Wander: [`play`](../reference/wander.md#audio-play), [`play_sound`](../reference/wander.md#audio-play_sound), [`stop_sound`](../reference/wander.md#audio-stop_sound), [`music`](../reference/wander.md#audio-music), [`set_volume`](../reference/wander.md#audio-set_volume).
- Related pages: [Simulation and time](simulation.md), [2D and UI](2d-ui.md).
- Design document: [docs/AUDIO.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/AUDIO.md).
