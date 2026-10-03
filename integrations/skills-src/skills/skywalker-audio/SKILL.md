---
name: skywalker-audio
description: Sound and input for Skywalker games - generate sound effects, ambience loops and music procedurally with audio_generate, verify them without ears using audio_info (LUFS, clipping, loop seams), wire spatial audio and the mixer, and define device-independent input actions with input_map. Use for any SFX, music, ambience, mix or control-scheme work.
---

# Audio and input

Agents cannot listen, so the loop is **generate, measure (`audio_info`), wire, test in simulation**. Generation is a pure
function of its parameters (same seed, same audio), needs no external service and no licensing, and works headless.

## Generate

```text
audio_generate {preset:"footstep_stone", variations:4, path:"audio/step.wav"}          # step_1..step_4
audio_generate {preset:"fire_crackle_loop", path:"audio/fire.wav", entity:"Campfire"}   # also creates/updates the audio component
audio_generate {music:{mood:"adventure", bars:16, tempo:110, energy:0.6}, path:"audio/theme.wav"}
audio_generate {list:true}                                                             # presets, moods and the full parameter reference
```

- **Presets**: jump, coin, pickup, powerup, error, hit, explosion, laser, shoot, swoosh, door, ui_click, ui_hover,
  footstep_grass / stone / wood, heartbeat, and seamless loops wind_loop, rain_loop, fire_crackle_loop, ocean_waves_loop, ambient_drone.
- Variety: `seed` or `variations` (1-16). Footsteps and impacts must vary or they sound mechanical; pick a variation at random in Wander.
- Adjust: `pitch` (multiplier on tonal layers), `volume`. Fully custom: `params:{layers:[{wave, freq, slide, attack, decay, lowpass, ...}], echo, reverb, loopSeconds}`
  (waves: sine square saw triangle noise pink brown fm grains; `loopSeconds` makes a seamless loop; typos get did-you-mean hints).
- **Music**: `music:{mood: calm|happy|sad|tense|epic|mysterious|eerie|adventure|battle, tempo 30-240, key:"A minor", progression:"i VI III VII",
  bars 2-64, energy 0-1, reverb, pad/bass/arp/melody/drums on|off}`. It loops exactly `bars` long. Make 2-3 moods per game and crossfade.
  For a bed under dialogue: `drums:false, melody:false` and a lower bus volume.
- Add `description` and `tags` so teammates can find the clip (`asset_list {type:"audio"}`).

## Verify by measurement

```text
audio_info {path:"audio/theme.wav"}
```

Returns duration, peak/RMS dBFS, **integrated LUFS**, clipping, DC offset, silence ratio, click risk at start/end, and for loops the jump at the
seam, plus plain-language warnings. Targets: effects -23 to -14 LUFS, music -20 to -16, ambience slightly lower, no clipping, loops
`seamless`. If a warning appears, regenerate with adjusted `volume`/`params`, do not ship around it.

## Wire into the game

| Mechanism | How |
|---|---|
| Emitter on an entity | `components:{audio:{clip, volume, pitch, loop, playOnStart, spatial, minDistance, maxDistance, rolloff: inverse\|linear\|exponential\|none, rolloffFactor, bus, doppler}}` |
| Ears | `listener` component on the player; without it, sound is heard from the active camera |
| Wander | `play(e)`, `stop_sound(e)`, `play_sound("audio/x.wav", vol)`, `music("audio/theme.wav", fade)`, `set_volume("music", 0.3)` |
| Mixer | `audio_mix {buses:{music:0.5, ambience:{volume:0.8}}, musicFade}` (stored in `audio.json`); `audio_mix {}` reports live voices, buses, distance gain |
| Audition | `audio_play {clip}` in edit mode (preview), or `sim_control play` and read `audio_mix` |

Buses: master, music, sfx, ambience, voice, ui (`ui` keeps playing while paused). Choose `spatial:false` for music and UI; `spatial:true`
with `maxDistance` ~25 for a campfire; `rolloff:"linear"` if you want true silence at the edge. Sound only plays while the game plays, and
playback never changes gameplay state (deterministic).

```wander
behavior Footsteps
  var walked = 0
  var last = vec(0, 0, 0)
  on start
    last = self.position
  end
  on tick
    walked = walked + distance(self.position, last)
    last = self.position
    if walked > 1.6 then
      walked = 0
      play_sound("audio/step_" + str(floor(random(1, 5))) + ".wav", 0.6)
    end
  end
end
```

Debug "why is it silent?": `audio_mix` (voices? bus muted? real device or null output?), then `audio_info` on the clip (silent file?), then the voice's
`distanceGain` (near 0: listener too far, raise `maxDistance` or use `inverse`). In CI, sound is mixed silently (null device); levels and panning are still real.

## Input maps

Gameplay reads **actions**, not keys, so keyboard, mouse and gamepad all work and players can rebind. The map lives in `input.json`.

```text
input_map {operation:"get", catalog:true}                                         # map, live state of every action, valid binding sources
input_map {operation:"set_action", name:"dash", type:"button", bindings:["key:shift","pad:east"]}
input_map {operation:"remove_action", name:"pause"}   input_map {operation:"reset"}
```

Types: `button` (held / pressed / released), `axis` (-1..1), `axis2d` (x right, **y forward/up**). Binding strings: `key:space`, `mouse:left`,
`mouse:delta`, `mouse:position`, `pad:south`, `pad:leftStick`, `pad:rightTrigger`; composites `wasd`, `arrows`, `dpad`, `ad`, `qe`; objects
`{source, scale, invertY, deadzone}`. Defaults: move (WASD/arrows/left stick), look (mouse/right stick), jump, fire, aim, interact (E),
sprint, pause, cursor. The strongest binding wins, so devices never add above 1.

Wander: `action("jump")` (held), `pressed("jump")`, `released("jump")`, `axis("move")` (vector `(x, y, 0)` for axis2d), `on action "jump"`.

Test like a player (60 ticks = 1 s; input applies on the next tick, so step after):

```text
sim_input {actions:["jump"]}                                  sim_control {action:"step", ticks:30}
sim_input {axes:[{name:"move", x:0, y:1, ticks:120}]}         sim_control {action:"step", ticks:120}
sim_trace {entities:["Player"], properties:["transform.position"], ticks:120, every:20}
```

## Pitfalls

- Shipping a clip that clips or has a click: always `audio_info`.
- Identical repeated one-shots: use variations. Loops: use presets ending in `_loop` or `loopSeconds`.
- `play_sound` with a missing file is a script runtime error (five of them disable the script): generate before wiring.
- Raw `key("space")` ignores rebinding; prefer actions unless you need a specific key.
- `set_volume` is a runtime override that ends when play stops; use `audio_mix` for the project mix.
