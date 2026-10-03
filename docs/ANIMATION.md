# Animation

Skywalker animates rigged characters with skeletal clips, state-machine **animators**,
look-at IK and bone attachments, and directs cinematics with **sequences** (tracks of keyed
properties, camera shots and cuts, events and animations). Everything is a reflected
component plus a set of tools, so agents and the editor drive it the same way.

| Piece | What it is | Where |
|---|---|---|
| Animation library | `*.anim`: one model's skeleton and clips (written at import) | `anim/Animation.h` |
| Controller | `*.animctl.json`: parameters, layers, states, blend spaces, transitions | `anim/Controller.h` |
| `animator` component | Plays a library on the entity's rigged meshes, through a controller or one clip | `ecs/AnimationComponents.h` |
| `attach` component | Keeps an entity on a bone (weapons, hats, lanterns) | same |
| `ik` component | Two-bone IK effector: a hand or foot reaches this entity | same |
| Sequence | `*.sequence.json`: a cinematic timeline | `anim/Sequence.h` |
| `sequencer` component | Plays a sequence during the simulation; previews it while editing | same |
| AnimationSystem | Runs all of the above, deterministically, in the fixed tick | `anim/AnimationSystem.h` |

## Importing characters

`asset_import` (or `asset_download`) of a glTF/GLB with skins or animations:

- writes `<name>.anim` next to the model: every node of the glTF scene becomes a bone,
  every glTF animation a clip (translation, rotation and scale channels; `LINEAR`, `STEP`
  and `CUBICSPLINE`). Exporter names are cleaned (`Armature|Walk` → `Walk`);
- keeps the mesh asset (`asset:models/hero.glb`, or one part per material
  `asset:models/hero.glb#2`) with its skinning data: `JOINTS_0/WEIGHTS_0` (and `_1`; the
  four strongest influences are kept), `u8`/`u16` joints, float or normalized weights,
  inverse bind matrices. Static parts under animated nodes follow their node rigidly;
- **keeps the real size** of rigged characters (pass `normalize` explicitly to override)
  and **turns them to face −Z**, Skywalker's forward (glTF characters face +Z), so
  `look self at ...` and `move self toward ...` behave;
- writes `<name>.prefab.json`: a ready-to-use character with an `animator` (playing an
  `idle`-like clip if there is one). `create_entity` instantiates it.

Animation-only files (a glTF with a skeleton and clips but no mesh) become clip libraries.
Clips are shared between rigs **by bone name** (`mixamorig:Hips` matches `Hips`): a
controller can use `"anims/dance.anim#Dance"` from another file. Only the hips keep their
translation when retargeting, scaled to the target's proportions.

`.meta` import settings record `animation`, `turnAround`, `normalize` and `zUp`, so the mesh
reloads identically. `asset_list type=animation|controller|sequence` lists the new assets.

## The animator component

| Field | Meaning |
|---|---|
| `library` | `*.anim`; empty = the library imported with the rigged mesh below this entity |
| `controller` | `*.animctl.json` state machine; empty = play `clip` |
| `clip`, `loop` | Without a controller: the clip to play (empty = the first) |
| `speed` | Playback rate (0 pauses) |
| `rootMotion` | Move the entity by the clips' root (hips) motion; the hips stay over the entity |
| `preview`, `time` | Editor only: `rest` (bind pose), `pose` (frame at `time` seconds into the default state) or `play` (live) |
| `lookAt`, `lookAtWeight`, `lookAtLimit` | Look-at IK: the head (and neck/spine) turn toward an entity, at most `lookAtLimit` degrees |

The animator lives on the character's root; rigged meshes anywhere below it are posed by
it (multi-material characters are a root with one child per part).

## Controllers (`*.animctl.json`)

```json
{
  "format": "skywalker.animctl", "version": 1,
  "library": "characters/hero.anim",
  "parameters": {"speed": "float", "jump": "trigger", "grounded": {"type": "bool", "default": true},
                 "aimX": "float", "aimY": "float", "wave": "float"},
  "layers": [
    {"name": "Base", "default": "Locomotion",
     "states": {
       "Locomotion": {"blend": {"parameter": "speed", "motions": [
           {"clip": "Idle", "at": 0}, {"clip": "Walk", "at": 1.4}, {"clip": "Run", "at": 3.9}]},
         "events": [{"time": 0.0, "name": "footstep"}, {"time": 0.5, "name": "footstep"}]},
       "Strafe": {"blend2d": {"x": "aimX", "y": "aimY", "motions": [
           {"clip": "Idle", "pos": [0, 0]}, {"clip": "WalkLeft", "pos": [-1, 0]}, {"clip": "WalkRight", "pos": [1, 0]},
           {"clip": "Walk", "pos": [0, 1]}, {"clip": "WalkBack", "pos": [0, -1]}]}},
       "Jump": {"clip": "Jump", "loop": false, "speed": 1.2},
       "Fall": {"clip": "Fall"}
     },
     "transitions": [
       {"from": "any", "to": "Jump", "when": "jump and grounded", "duration": 0.1},
       {"from": "Jump", "to": "Locomotion", "exit": 0.9, "duration": 0.2},
       {"from": "any", "to": "Fall", "when": "!grounded", "duration": 0.3, "interruptible": true}
     ]},
    {"name": "Wave", "mask": ["RightShoulder"], "weightParameter": "wave", "default": "Wave",
     "states": {"Wave": {"clip": "Wave"}}}
  ]
}
```

- **Parameters**: `float`, `int`, `bool`, `trigger`. A trigger stays armed until a
  transition consumes it, or for 0.25 s.
- **States**: `clip`, `blend` (1D on one parameter, thresholds `at`) or `blend2d` (two
  parameters, positions `pos`, gradient-band weights). Blended clips share one normalized
  time, so feet stay in sync; the cycle length blends too. Optional `speed`,
  `speedParameter` (a multiplier), `loop`, `events` (normalized `time` 0..1, `name`).
- **Transitions**: `from` (a state or `any`; any-state transitions are checked first),
  `to`, `when` (a string such as `"speed > 0.1 and !crouch"`, a list of such strings, or
  `{param, op, value}` objects; a bare trigger name fires and consumes it), `duration`
  (crossfade seconds), `exit` (normalized exit time; transitions without conditions leave at
  the end), `offset` (start time in the destination), `interruptible` (a later transition
  may cut in; it blends from the pose on screen).
- **Layers**: after the base layer, each layer overrides (or with `"blending": "additive"`
  adds to) the pose with a `weight` or `weightParameter`, optionally only for `mask` bones
  and their descendants (upper-body actions over locomotion).
- Every reference is validated (`animator_setup`) with did-you-mean errors: clips, states,
  parameters (and their types), mask bones.

Runtime rules worth knowing:

- Animators update in the fixed 1/60 s tick, after Wander and the sequencers. Identical
  inputs give bit-identical poses; stopping the simulation resets everything.
- `play_animation` / `animator_set play` crossfade to a state, or to any clip as a one-shot:
  a non-looping clip played this way returns to the default state when it ends. Playing the
  state that is already playing does nothing, so calling it every tick is harmless.
- Animation events reach Wander as `on anim "footstep"` on the animator's entity (the same
  as `on event "anim:footstep"`), on the next tick.
- Root motion is the hips' horizontal movement (relative to the model's up axis); vertical
  motion stays in the pose. The delta goes to the physics hook
  (`AnimationSystem::hooks.rootMotion`, for a character controller) or moves the Transform.

## Bone attachments

`bone_attach {"entity": "Sword", "to": "Hero", "bone": "RightHand", "offset": [0, 0.08, 0]}`
adds an `attach` component (`character`, `bone`, `offset`, `rotation`, `followScale`) and parents
the prop under the character. Attachments follow the bone every tick while playing and in
the editor preview (applied per frame, never saved). Bone names match fuzzily
(`righthand`, `mixamorig:RightHand`); `animation_list bones=true` lists them.

## IK

**Look-at** (animator fields `lookAt`, `lookAtWeight`, `lookAtLimit`): the head, neck and up
to two spine bones share the turn toward an entity (another character's head, else the
middle of a mesh), clamped to a cone around the body's facing, smoothed while playing.

**Two-bone IK** (`ik` component on an *effector* entity: `character`, `bone`, `weight`,
`pole`, `matchRotation`): the end bone (hand, foot) reaches the effector; its parent and
grandparent (elbow and shoulder, knee and hip) bend analytically and keep their lengths;
out-of-reach targets straighten the limb toward them. `pole` is the bend direction in the
character's space (knees `[0, 0, -1]`); zero keeps the animated bend plane. `bone_ik` creates
an effector (or turns an existing object into one). Move or keyframe effectors like any
entity — a reach in a cutscene is a `transform.position` track on the effector. IK runs
after the state machine and look-at, every tick while playing and in editor previews.

## Sequences (`*.sequence.json`)

```json
{
  "format": "skywalker.sequence", "version": 1, "name": "Intro", "duration": 12,
  "tracks": [
    {"type": "property", "entity": "Sun Lamp", "property": "light.intensity",
     "keys": [{"t": 0, "value": 0}, {"t": 3, "value": 6, "ease": "smooth"}]},
    {"type": "property", "property": "environment.sunElevation",
     "keys": [{"t": 0, "value": 4}, {"t": 12, "value": 30, "ease": "ease_out"}]},
    {"type": "shot", "camera": "Cam A", "keys": [
      {"t": 0, "shot": "orbit", "duration": 6, "target": "Hero", "offset": [0, 1.4, 0], "radius": 5, "height": 0.4, "from": -40, "to": 40},
      {"t": 6, "shot": "dolly", "duration": 6, "target": "Hero", "distance": [6, 2], "angle": 180, "fov": [50, 30]}]},
    {"type": "camera", "keys": [{"t": 0, "camera": "Cam A"}, {"t": 9, "camera": "Closeup"}]},
    {"type": "event", "keys": [{"t": 4, "event": "doors_open", "target": "Gate"}]},
    {"type": "animation", "entity": "Hero", "keys": [{"t": 0, "play": "Walk"}, {"t": 6, "play": "Wave", "fade": 0.3},
                                                     {"t": 8, "params": {"speed": 0}}]}
  ]
}
```

| Track | Keys | Notes |
|---|---|---|
| `property` | `t`, `value`, `ease` | Any reflected field: `transform.position/rotation/scale`, `light.*`, `camera.fov`, `mesh.color/emissive`, `particles.rate`, `environment.*` (no entity). Numbers, vectors and colors interpolate; bools and strings step. |
| `camera` | `t`, `camera` | The live camera from each key on (sets `camera.primary`). |
| `shot` | `t`, `duration`, `shot`, parameters | Procedural camera moves, evaluated every tick (they follow moving targets). |
| `event` | `t`, `event`, `target?` | Wander events. |
| `animation` | `t`, `play?`, `fade?`, `loop?`, `layer?`, `params?` | Drives an animator. |

**Easing** (the segment after a key): `linear`, `step`, `smooth`, `ease_in`, `ease_out`,
`auto` (a Catmull-Rom spline through the neighbouring keys, best for camera paths) or
`[x1, y1, x2, y2]` (CSS cubic-bezier).

**Shots**: `orbit` (around the target: `radius`, `height`, `from`/`to` degrees, 0 = +Z of the
target), `dolly` (`distance` [far, near] along `angle`, or `from`/`to` points), `crane`
(`height` [low, high]), `track` (side-on at `angle` and `distance`, sliding `from`/`to`
meters while following the target), `pan` (fixed `position`; looks `from`/`to` entities,
points or yaw degrees), `static` (fixed `position` looking at the target), `path`
(Catmull-Rom through `points`, looking at the target or along the path). All take `target`
(entity or point; entities are aimed at their bounds center), `offset`, `fov` (number or
[from, to]), `roll` and `ease` (default `smooth`).

The `sequencer` component (`sequence`, `playOnStart`, `loop`, `speed`, `preview`, `time`)
plays the sequence when the simulation starts (or on `sequence_play` / Wander
`play_sequence(e)`). While editing, `preview` shows it frozen at `time` (the timeline bar in
the Details panel scrubs it); like every editor preview it is applied per frame and never
saved.

## Tools

| Tool | Use it to |
|---|---|
| `animation_list` | See a character's clips (with root speeds), controller, state, events — or a model's skeleton, or every library |
| `animator_setup` | Create and assign a controller: `preset: locomotion` (speed blend Idle→Walk→Run at the clips' real speeds, jump trigger), `clips`, or a full validated document |
| `animation_preview` | Render a character at clip/state times (contact sheets); auto-framed |
| `animator_set` | Set parameters / triggers, play states or clips (live while playing, preview while editing), look-at, preview mode |
| `bone_attach` | Attach a prop to a bone |
| `bone_ik` | Make a hand or foot reach an object or point (two-bone IK effector) |
| `sequence_create` | New sequence asset + an entity that plays it |
| `sequence_key` | Add/replace many keys at once: properties, camera cuts, events, animation keys |
| `sequence_camera_shot` | Add an orbit/dolly/crane/track/pan/static/path shot (creates the camera, adds the cut) |
| `sequence_get` | Tracks, length, values and live camera at a time |
| `sequence_scrub` | Storyboard captures through the sequence's camera, frame rendering to disk (`fps`, `save_dir`), editor preview |
| `sequence_play` | Play / stop (starts the simulation when editing) |

## Wander

```wander
behavior Hero
  intent "Walk with WASD, jump on space, wave on E, footsteps make dust."
  on tick
    let v = 0
    if key("w") then
      v = 1.4
      move self by (forward(self) * 1.4 * dt)
    end
    set_param(self, "speed", v)
  end
  on key "space"
    trigger(self, "jump")
  end
  on key "e"
    play_animation(self, "Wave", 0.2)
  end
  on anim "footstep"
    burst(find("Dust"), 6)
  end
end
```

`set_param(e, name, value)`, `trigger(e, name)`, `play_animation(e, name, fade?, loop?)`,
`anim_state(e)` (the base layer's state name) and `play_sequence(e, from?)`. Errors carry
did-you-mean hints. The `animator` fields are also properties: `self.animator.speed = 0.5`,
`self.animator.lookAt = "Player"`.

## Agent recipes

**Import a character and make it walk**

1. `asset_import {"path": "characters/knight.glb", "create_entity": "Knight", "position": [0, 0, 0]}`
   → the result lists `clips` and the `prefab`.
2. `animation_list {"entity": "Knight"}` → clip names, durations, `rootSpeed` of walk/run.
3. `animator_setup {"entity": "Knight"}` (or `"root_motion": true` to move by the clips) →
   `speed` blends idle/walk/run at their real speeds; `jump` trigger if there is a jump clip.
4. `animation_preview {"entity": "Knight", "clip": "Walk", "times": [0, 0.25, 0.5, 0.75]}`
   → check the cycle.
5. `behavior_set` with the Wander above, `sim_control play`, `sim_input {"hold": ["w"]}`,
   `sim_control step`, `viewport_capture`.
6. Props: `bone_attach {"entity": "Sword", "to": "Knight", "bone": "RightHand"}`; head
   tracking: `animator_set {"entity": "Knight", "look_at": "Player"}`.

**A cutscene with camera moves**

1. `sequence_create {"path": "cinematics/arrival.sequence.json", "duration": 10, "entity": "Director"}`
2. `sequence_camera_shot {"sequence": "Director", "camera": "Cam Wide", "shot": "crane", "target": "Gate", "duration": 4, "height": [0.5, 8], "distance": 14}`
3. `sequence_camera_shot {"sequence": "Director", "camera": "Cam Close", "shot": "orbit", "target": "Knight", "offset": [0, 1.5, 0], "start": 4, "duration": 6, "radius": 3, "from": 200, "to": 160, "fov": [40, 30]}`
4. `sequence_key {"sequence": "Director", "animations": [{"entity": "Knight", "t": 0, "play": "Walk"}, {"entity": "Knight", "t": 5, "play": "Wave", "fade": 0.3}], "keys": [{"property": "environment.sunElevation", "t": 0, "value": 5}, {"property": "environment.sunElevation", "t": 10, "value": 25, "ease": "smooth"}], "events": [{"t": 4, "event": "gate_open", "target": "Gate"}]}`
5. `sequence_scrub {"sequence": "Director", "times": [1, 3.5, 5, 8]}` → a storyboard
   through the live cameras. Adjust, scrub again.
6. `sequence_scrub {"sequence": "Director", "fps": 24, "save_dir": "renders/arrival"}`
   renders frames for review or video; `sequence_play {"sequence": "Director"}` runs it in
   the simulation.

## Rendering

- **GPU skinning** (Metal) is a compute pre-pass (`Skinning.metal`): each skinned draw's
  posed vertices go into a per-instance buffer registered under a unique mesh key
  (`<mesh>@skin<entity>`), so the standard pipelines — main pass, shadows, toon and
  selection outlines, alpha cutouts — draw animated characters unchanged. Parts of one
  character share one joint-matrix buffer per frame. Linear blend skinning, 4 influences.
- **CPU skinning** (`anim::skinMesh`) gives raycasts and `place_on_surface` the posed
  geometry; the CPU renderer and picking use the posed bounds.
- **Culling bounds** are exact-conservative per pose: each joint's bind-space vertex bounds
  through its joint matrix.
- `FrameData::skins` / `DrawItem::skin` carry the palettes; other backends implement the
  same contract (or draw the rest pose).

## Limits (v0.1)

- No morph targets (blend shapes), no sparse accessors, no `KHR_animation_pointer`.
- IK: look-at and two-bone effectors; no automatic foot planting on terrain (place foot
  effectors from gameplay raycasts), no full-body IK.
- Root motion is translation only (no root yaw).
- Retargeting is by bone name with hips-translation scaling (no pose-space retargeting).
- Sequencer `mesh.color` / `mesh.emissive` keys show on entities with inline surfaces; an
  entity using a material asset (`mesh.material`) takes its look from the asset.

## Test models

Unit tests generate tiny rigged glTF files in code (`tests/anim_fixtures.h`). For manual
checks, permissively licensed rigged samples work well; keep them in the gitignored
`samples/` folder:

- Khronos glTF-Sample-Assets (check each model's license; e.g. `CesiumMan`, `RiggedFigure`,
  `RiggedSimple`, `Fox` are CC-BY 4.0 — credit the authors):
  `https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models`
- Quaternius animated characters (CC0): `https://quaternius.com`
