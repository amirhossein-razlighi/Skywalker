---
name: skywalker-animation
description: Animate characters and direct cinematics in Skywalker - import rigged glTF, animator state machines and blend trees, driving them from Wander, bone attachments (weapons, hats), look-at and two-bone IK, and sequences with keyed properties, camera shots and cuts, storyboard scrubs, and final movie renders to MP4/ProRes/PNG with motion blur (movie_render). Use for walking/idle/jump characters, held props, hand-on-handle reaches, cutscenes, trailers and films.
---

# Animation and sequences

Load skywalker-core first (and skywalker-assets for importing). Engine doc: `skywalker://docs/ANIMATION`. Everything is deterministic: animators update in the fixed 1/60 s tick and
stopping the simulation resets them.

## Pieces

| Piece | What it is |
|---|---|
| `*.anim` library | A model's skeleton and clips; written when a rigged glTF/GLB is imported |
| `*.animctl.json` controller | Parameters, layers, states (clip, 1D `blend`, `blend2d`), transitions with conditions |
| `animator` component | On the character root; poses every rigged mesh below it (`library`, `controller`, `clip`, `speed`, `rootMotion`, `lookAt`) |
| `attach` component | Keeps a prop on a bone |
| `ik` component | Two-bone effector: a hand or foot reaches this entity |
| `*.sequence.json` + `sequencer` component | A cinematic timeline played in the simulation, previewable while editing |

Clips are shared between rigs **by bone name**; only the hips keep translation when retargeting. No morph targets (blend shapes) yet.

## Workflow A: import a character and make it walk

1. **Import**: `asset_import {path, create_entity, position}` of a `.glb/.gltf` with skins. The result lists `clips`, the `.anim`, and a ready `prefab`. Rigged characters keep their real size and are turned to face -Z.
2. **Inspect**: `animation_list {entity}` gives clip names, durations and `rootSpeed` (m/s) of locomotion clips. `animation_list {model, bones:true}` lists bone names; with neither argument it lists every library.
3. **State machine**: `animator_setup {entity}` builds the `locomotion` preset (parameter `speed` drives Idle > Walk > Run at each clip's real speed, so feet do not slide; a `jump` trigger if there is a Jump clip).
4. **Check the cycle**: `animation_preview` renders contact sheets, auto-framed.
5. **Drive it** from Wander (`behavior_set`) and verify with the simulation.

```text
asset_import {path:"characters/knight.glb", create_entity:"Knight", position:[0,0,0]}
animation_list {entity:"Knight"}
animator_setup {entity:"Knight", root_motion:false}
animation_preview {entity:"Knight", clip:"Walk", times:[0,0.25,0.5,0.75], view:"side", save_path:"walk_sheet.png"}
animator_set {entity:"Knight", params:{speed:3}, trigger:"jump"}
animator_set {entity:"Knight", play:"Wave", fade:0.3}
behavior_set {entity:"Knight", name:"Hero", intent:"Walk with W, jump on space, wave on E, footsteps raise dust.", source:"behavior Hero\n  on tick\n    let v = 0\n    if key(\"w\") then\n      v = 1.4\n      move self by (forward(self) * 1.4 * dt)\n    end\n    set_param(self, \"speed\", v)\n  end\n  on key \"space\"\n    trigger(self, \"jump\")\n  end\n  on key \"e\"\n    play_animation(self, \"Wave\", 0.2)\n  end\n  on anim \"footstep\"\n    log \"step\"\n  end\nend"}
sim_input {hold:["w"]}
sim_control {action:"step", ticks:60}
animation_list {entity:"Knight"}
viewport_capture {eye:[3,1.6,4], target:[0,1,0], samples:8}
sim_control {action:"stop"}
```

- `animator_setup` options: `preset:"clips"` (one state per clip, drive with `play_animation`), `clips:{idle:"Breathe", run:"Sprint"}` to override name matching, `library` when the character has no `.anim` yet,
  `path` for the controller file, `root_motion:true` to move by the clips' hips motion, or a full `controller` document (validated, did-you-mean errors).
- Controller essentials: parameters `float|int|bool|trigger` (a trigger stays armed until consumed, or 0.25 s); transitions `{from:"any"|state, to, when:"speed > 0.1 and !crouch", duration, exit, interruptible}`;
  extra layers can `mask` bones (upper-body actions over locomotion) with `weightParameter` or `"blending":"additive"`. Full example: `skywalker://docs/ANIMATION`.
- `animator_set` while **editing** only previews (nothing is saved, parameters set then are preview-only); while **playing** it acts live like `set_param` / `trigger` / `play_animation`.
  `play` crossfades to a state, or to any clip as a one-shot that returns to the default state; replaying the current state is a no-op, so calling it every tick is safe.
- Animation events (controller `events`, clip events) reach Wander as `on anim "footstep"` on the next tick.
- Root motion is the hips' horizontal movement only (no yaw). With a physics `character` it becomes that controller's desired velocity (collides, climbs steps); otherwise it moves the Transform.

### Characters and clip libraries from different files

When the character and its animations come from different glTF files (a base mesh plus a shared clip library), set the
animator's `library` to the **character's own** `.anim` (written at import, next to the mesh) and name every clip with its
library: `"anims/ual1.anim#Walk_Loop"`. Clips are then retargeted onto the character's skeleton by bone name (rotations, plus
the hips translation scaled to its proportions), so each body keeps its own bone lengths. Using the clip library's skeleton
as the animator library instead poses the mesh with the *other* rig's proportions (stretched necks, sunk hips). A rig
re-exported by a DCC must keep the source's root and hips frames (exporters sometimes fold a root rotation into the hips).

## Props, look-at and IK

```text
bone_attach {entity:"Sword", to:"Knight", bone:"RightHand", offset:[0,0.08,0.02], rotation:[0,90,0]}      # bone names match fuzzily; offsets are in bone space
animator_set {entity:"Knight", look_at:"Player"}                                                         # head/neck/spine turn toward an entity ("" clears), clamped by lookAtLimit
bone_ik {character:"Knight", bone:"LeftHand", entity:"Door Handle", weight:1}                            # an existing object becomes the effector
bone_ik {character:"Knight", bone:"RightFoot", position:[0.2,0.3,-0.4], pole:[0,0,-1], name:"Step IK"}   # or create a new effector at a world position
```

- `animation_list {entity, bones:true}` lists the bones first; a bad name returns did-you-mean.
- `bone_attach` parents the prop under the character (disable with `parent:false`); it follows the bone every tick and in the editor preview, and the attachment is never baked into the file.
- IK: `pole` is the bend direction in the character's space (knees `[0,0,-1]`, elbows usually `[0,0,1]`); out-of-reach targets straighten the limb. Animate a reach by keying the effector's `transform.position` with `sequence_key`.
  No automatic foot planting: raycast from Wander and move the foot effectors yourself.

## Workflow B: a cutscene

1. `sequence_create {path:"cinematics/arrival.sequence.json", duration:10, entity:"Director"}`: the asset plus an entity with a `sequencer` (plays on simulation start unless `play_on_start:false`).
2. **Camera moves**: `sequence_camera_shot` (creates missing cameras and adds the cut). Shots: `orbit` (`radius`, `height`, `from`/`to` degrees, 0 = +Z of target), `dolly` (`distance` [far,near], `angle`), `crane` (`height` [low,high]),
   `track`, `pan`, `static`, `path` (`points`), `flyover` (straight aerial pass over the target from the `angle` side: `distance` = half the pass, `height`). All take `target` (entity name or point), `offset`, `fov` (number or [from,to]), `roll`, `ease`, `start`, `duration`.
3. **Everything else**: `sequence_key` adds many keys at once: property keys (`transform.position`, `light.intensity`, `camera.fov`, `mesh.color`, `particles.rate`, `environment.sunElevation` with no entity), `camera_cuts`, `events`
   (Wander events, optional `target`) and `animations` (`play`, `fade`, `params`).
4. **Storyboard**: `sequence_scrub {times:[...]}` renders up to 8 frames through the sequence's own camera without playing, then restores the scene.
5. Adjust and scrub again; finally `sequence_play` (starts the simulation when editing) and check with `step` plus `viewport_capture {view:"scene"}`.
6. **Render the movie**: `movie_render` (below).

```text
sequence_create {path:"cinematics/arrival.sequence.json", duration:10, entity:"Director"}
sequence_camera_shot {sequence:"Director", camera:"Cam Wide", shot:"crane", target:"Gate", start:0, duration:4, height:[0.5,8], distance:14}
sequence_camera_shot {sequence:"Director", camera:"Cam Close", shot:"orbit", target:"Knight", offset:[0,1.5,0], start:4, duration:6, radius:3, from:200, to:160, fov:[40,30]}
sequence_key {sequence:"Director", animations:[{entity:"Knight", t:0, play:"Walk"}, {entity:"Knight", t:5, play:"Wave", fade:0.3}], keys:[{property:"environment.sunElevation", t:0, value:5}, {property:"environment.sunElevation", t:10, value:25, ease:"smooth"}], events:[{t:4, event:"gate_open", target:"Gate"}]}
sequence_get {sequence:"Director", time:5}
sequence_scrub {sequence:"Director", times:[1,3.5,5,8]}
sequence_scrub {sequence:"Director", fps:24, from:0, to:10, save_dir:"renders/arrival", include_image:false}
sequence_play {sequence:"Director", action:"play", from:0}
```

- Easing: `linear` (default for keys), `step`, `smooth`, `ease_in`, `ease_out`, `auto` (Catmull-Rom through neighbours, best for camera paths) or `[x1,y1,x2,y2]`. Numbers, vectors and colors interpolate; booleans and strings step.
- A key at an existing time replaces it; `sequence_key {replace:true}` clears the touched tracks first; `remove:[{entity, property}]` deletes tracks. `sequence_get` shows tracks, length, values and the live camera at a time.
- `sequence_scrub` works only while editing. `persist:true` leaves the editor showing that time; `save_dir` must exist (`frame_0000.png`, ...). In Wander: `play_sequence(e, start?)`.
- Sequencer `mesh.color` keys show only on entities with inline surfaces, not ones using a `mesh.material` asset.

## Workflow C: render a movie (the movie renderer)

`movie_render` renders offline and deterministically from the scene state (restored afterwards): a sequence (its whole length, or `start`/`end`), an inline
`camera` move, or the scene camera for `duration` seconds. Engine doc: `skywalker://docs/MOVIE_RENDER`.

```text
movie_render {sequence:"Director", output:"renders/arrival.mp4", resolution:"1080p", fps:24, samples:8, shutter:0.5}
movie_render {sequence:"Director", outputs:["renders/arrival_master.mov", {path:"renders/arrival_web.mp4", codec:"hevc"}], samples:16, shutter:0.5, simulate:true}
movie_render {camera:{keys:[{t:0, eye:[-3,2.7,-27], target:[2,2,-22]}, {t:4, eye:[-11,9,-9], target:[20,4,38]}]}, simulate:true, output:"renders/reveal/frame_####.png"}
movie_render {camera:{shots:[{shot:"flyover", target:"Village", duration:6, distance:40, height:15}, {shot:"orbit", target:"Well", duration:4, radius:8}]}, clay:true, output:"renders/clay.mov"}
movie_render {output:"renders/reveal/frame_####.png", resume:true, camera:{keys:[{t:0, eye:[-3,2.7,-27], target:[2,2,-22]}, {t:4, eye:[-11,9,-9], target:[20,4,38]}]}, simulate:true}
movie_render {action:"status"}
```

- Outputs by extension: `.mp4` = H.264 (`codec:"hevc"` = 10-bit HEVC), `.mov` = ProRes 422 HQ, a folder or `name_####.png` = PNG sequence. Several outputs come from one render.
- `simulate:true` runs the game (scripts, physics, water bobbing boats); the default runs only sequences, animators and particles.
- Motion blur: `shutter` 0..1 of the frame (0.5 = 180 degrees; default = the camera's `motionBlur`), sub-frames at exact fractional times; `samples` is the per-frame budget (8 previews, 16-32 finals).
- Same move in three looks for "sketch to clay to final" transitions: render it with `debug_view:"sketch"`, `clay:true` and normally; the frames line up exactly.
- Skinned characters write per-vertex motion vectors (their previous pose), so TAA and MetalFX keep them sharp in motion. To check, `sim_control {action:"step", ticks:1}` then `viewport_capture {samples:1, debug_view:"motion"}`: moving limbs are colored, the static set stays gray.
- Long renders: `background:true` returns at once (the editor shows a progress bar; poll with `action:"status"`); `action:"cancel"` stops after the current sub-frame and keeps what was written; `resume:true` continues a PNG sequence.
- Preview a range cheaply first (`quality:"fast"`, `samples:1`, `resolution:"720p"`), then render the final.

## Wander builtins

`set_param(e, name, value)`, `trigger(e, name)`, `play_animation(e, name, fade?, loop?)`, `anim_state(e)`, `play_sequence(e, from?)`; fields are also properties: `self.animator.speed = 0.5`, `self.animator.lookAt = "Player"`.

## Verification loop

1. `animation_preview` with 4-8 `times` across the clip: check the pose reads (feet on the ground, arms not through the body), loops are seamless (first and last frame match).
2. `animation_list {entity}` while playing shows the current state, parameters and recent events; `sim_trace` on `transform.position` confirms speed matches the clip's `rootSpeed`.
3. Capture mid-run at fixed ticks (`sim_control step`, then `viewport_capture`) from two angles. Judge motion from several frames, never one.
4. For cutscenes, scrub a storyboard, check every cut lands on the subject and the last frame is clean, then `movie_render` the video (check a few frames of the PNG sequence or the summary's timings).

## Pitfalls

- Parameters set with `animator_set` while editing are preview-only and reset on play; set them in Wander or while playing.
- Wander `set_param` every tick overrides your manual `animator_set` value on the same tick.
- `no animation library` error: the character has no rigged mesh below it; import the model first or pass `library` to `animator_setup`.
- Animator lives on the root; skinned parts must be descendants of that entity.
- Sliding feet: set `speed` to the real velocity (`rootSpeed` from `animation_list`), or use `root_motion`.
- A sequence shot with `cut:false` does not become the live camera; add a `camera_cuts` key or leave `cut` at its default.
- Mixamo-style names (`mixamorig:Hips`) are fine; clip names are cleaned (`Armature|Walk` becomes `Walk`).
