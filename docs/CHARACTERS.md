# Characters

Everything that makes a third-person character look and move like a person (or a furred
beast): humanoid bone maps, pose-space retargeting, root motion with turning, automatic foot
planting and hand targets, directional blend spaces and turn in place, hair and fur that ride
the animated skin, and the skin / eye / cloth / hair-card material models. The skeletal
animation basics (libraries, animators, controllers, attachments) are in
[ANIMATION.md](ANIMATION.md); strand rendering in [HAIR_AND_VFX.md](HAIR_AND_VFX.md).

| Piece | What it is | Where |
|---|---|---|
| Humanoid map | Slots (hips, spine, chest, neck, head, shoulders, arms, hands, legs, feet, toes) found in any skeleton | `anim/HumanoidMap.h` |
| Retargeting | Clips from one humanoid rig played on another, in pose space | `anim/Retarget.h` |
| `characterIk` component | Foot planting, pelvis, contact locks, hand targets, turn speed | `ecs/CharacterComponents.h`, `anim/CharacterIk.h` |
| Body colliders | Capsules fitted to a skinned mesh per bone (hair and fur collide with them) | `anim/BodyColliders.h` |
| Skinned grooms | Strand roots bound to triangles of the posed skin | `fx/GroomBinding.h` |
| Material models | `skin`, `eye`, `cloth`, `hair_card` shading | `assets/Material.h`, `shaders/Characters.metal` |

## Start here: `character_inspect`

`character_inspect {"entity": "Hero"}` reports, in one call: the humanoid map (slot → bone,
convention `mixamo` / `suffixed` / `generic`, confidence, missing slots, warnings), the world
position of key bones, every clip with its root speed (m/s) and root turn (degrees per cycle),
the root-motion settings, how `retargetFrom` clips map, the last foot / hand IK solve (contact,
lock, ground offset, slope, pelvis drop, hand reach error), every mesh part (bounds, skinned or
not, material model) and every groom (follows the skin?, bound roots, colliders).

## Humanoid bone maps

`detectHumanoid` finds the slots by name *and* topology: names are tokenized (`mixamorig:LeftUpLeg`,
`thigh_l`, `L_Thigh`, `Bip01 L Thigh`, `upperleg.L`), finger, twist, IK-control, pole and end
bones are skipped, sideless names are split by their rest position (+X is left in a rig facing
+Z), a chain of the wrong length is repaired from the hierarchy (IK foot controls that do not
belong to the leg are dropped), and the hips are the legs' common ancestor even when a bone
named "hips" is only a spine segment. `complete` = every core slot found; `retargetable` = the
slots pose-space retargeting needs. Fix a wrong guess with overrides
(`source_map` / `target_map` on `animation_retarget`: `{"leftHand": "L_Wrist"}`).

## Retargeting (`animation_retarget`, `animator.retargetFrom`)

Pose space: for every mapped bone the source's rotation *relative to its rest pose* is carried
over in world space and re-applied on the target's rest pose, so different bone names, bone
axes and rest poses (T-pose vs A-pose) all line up. The hips move by the source motion scaled by
the leg-length ratio (hip height when legs are missing), so a short character does not slide;
bones keep their own lengths (nothing stretches). Unmapped bones (fingers, twist, props) keep
their rest pose; fingers map by name when both rigs share them.

```jsonc
// Look before writing: both maps, the alignment, warnings.
animation_retarget {"source": "anims/pack.anim", "target": "Knight", "preview": true}
// Bake clips onto the target's skeleton (default output: <target>_<source>.anim next to the target).
animation_retarget {"source": "anims/pack.anim", "target": "Knight", "clips": ["Walk", "Run", "Idle"]}
```

Or retarget on the fly: `animator.retargetFrom = "anims/pack.anim"` makes clips that are not in
the character's own library come from the pack (controllers and `play_animation` can name them).
`animator.retarget`: `auto` (by name when every target slot bone exists in the source, else in
pose space), `pose`, or `name` (copy local rotations by bone name: only for identical rigs).
Each clip's result reports `stretch` (how far bones moved off their lengths, ~0 when correct).

## Root motion, turning and in place

| Animator field | Meaning |
|---|---|
| `rootMotion` | Move the entity by the clip's root (hips) motion |
| `rootYaw` | Also turn the entity by the root's rotation about up (turn clips, curved walks); the pose is de-rotated so the body does not turn twice |
| `inPlace` | Remove the root's horizontal motion and yaw: locomotion plays on the spot (gameplay moves the entity) |

`animation_list` / `character_inspect` show each clip's `rootTurn` in degrees, so an agent can
pick 90° and 180° turn clips by measurement.

## Directional locomotion and turn in place

`animator_setup {"entity": "Hero", "preset": "directional"}` builds a 2D blend space driven by
`moveX` (right) and `moveY` (forward) in m/s, with idle, forward, back, left, right and the
diagonals at their *measured* root velocities. The blend is directional (polar gradient bands):
direction and speed blend separately, so strafing between directions keeps its speed and the
feet do not slide. In a controller document that is `"blend2d": {"mode": "directional", ...}`
(the default `cartesian` is the classic gradient-band blend).

`turn_in_place(self, 90)` (Wander), `character_ik {"turn_to": 90}` or `{"turn_to": "Door"}` turn a
standing character at `characterIk.turnSpeed` (deg/s). Planted feet stay locked and re-plant in
small steps; the controller gets `turn` (degrees left) and `turning` parameters to play turn
clips.

## Foot and hand IK (`characterIk`, `character_ik`)

Feet probe the ground under them every tick (terrain, static meshes, physics bodies; never the
character itself), within `stepHeight`. Each foot moves to its ground, the pelvis drops by what
the lower leg cannot reach (`pelvis`), feet pitch and roll onto the slope up to `maxSlope` and
the toes clear the ground (`alignFeet`). A foot in **contact** locks where it landed (no sliding)
and re-plants with a short arc once the animation pulls it `lockDistance` away. Contact comes
from `contact`: `auto` (low and slower than `lockSpeed`), `velocity`, `events` (animation events
`foot_l_down`, `foot_l_up`, `foot_r_down`, `foot_r_up`), `always`, `never`. Everything is
smoothed (`smoothing`, 1/s) and deterministic; it runs after the state machine and look-at.

Hands reach entities: `leftHand` / `rightHand` with weights (animate the weights for grabs).
The target is evaluated after bone attachments of the same tick, so a grip point on a staff
held by the other hand never lags. A target beyond the arm's reach swings the clavicle toward
it; the remaining distance is reported as the hand's `error` (m).

```jsonc
character_ik {"entity": "Monk", "feet": true, "step_height": 0.4}
character_ik {"entity": "Monk", "left_hand": "Staff Grip"}            // two-handed staff
character_ik {"entity": "Climber", "left_hand": "Ledge L", "right_hand": "Ledge R", "hand_rotation": true}
```

Wander: `foot_ik(self, on, weight?)`, `hand_ik(self, "left", find("Staff Grip"), 1)`,
`hand_ik(self, "left", none)`, `turn_in_place(self, yaw_or_point)`, `look_at(self, target, weight?)`.

## Hair and fur on animated characters

On a rigged mesh a groom's roots are **bound to triangles** (vertex indices + barycentric
coordinates, plus a rest frame per guide). Every frame the GPU skinning output feeds a root
kernel that rebuilds root positions and frames on the posed skin (the CPU skinning path does the
same for the CPU renderer), so strands stay rooted while the character runs. Guides simulate in
substeps with the roots interpolated across the frame; `follow` (0..1) is the share of the
skin's motion strands take rigidly before simulating, `maxSpeed` caps strand speed relative to
the roots (no explosions on teleports or fast turns), and length constraints keep strands
inextensible. With `bodyColliders` the strands collide with up to 16 capsules fitted to the
skeleton (head, neck, chest, shoulders, arms), so hair falls over the shoulders instead of
through them.

| Field | Meaning |
|---|---|
| `attach` | `auto` (skinned when the mesh is rigged), `rigid`, `skinned` |
| `maskBone` | Grow only on vertices skinned to this bone and its children (`Head`) |
| `maskSpace`, `maskCenter`, `maskRadius`, `maskMirror` | An ellipsoid region: `bounds` (-1..1 of the bone's vertices: rig independent), `bone` (m from the bone), `mesh` (m); mirrored across X for brows and sideburns |
| `lodBias` | Real time: more (> 1) or fewer (< 1) strands at a distance |

Presets for rigged characters: `hair_scalp` (dense scalp hair on the Head bone), `beard`,
`eyebrows` (regions in the head's bounds), `fur_dense` (a creature's coat).

**Grooms stack.** A character usually wears several: scalp hair, a beard, brows. Each groom lives
on its own entity: the first `groom_create` on a mesh puts it on that mesh entity, every further
one creates a child named after the preset ("Beard", "Eyebrows"; or `name`) that grows on the
same mesh and follows the same skin. On a character root without a mesh the grooms grow on its
largest skinned part (the body). The result's `entity` is the groom's entity: change it with
`groom_update`, hide it with `entity_update {groom: {visible: false}}`, remove it with
`entity_delete`; `replace: true` swaps the mesh entity's own groom instead of adding one.
`groom_info` on the character (or any entity without a groom of its own) lists every groom on
it; `character_inspect` lists them under `grooms`.

```jsonc
groom_create {"entity": "Hero", "preset": "hair_scalp", "overrides": {"melanin": 0.85, "length": 0.12}}  // -> "Hair Scalp"
groom_create {"entity": "Hero", "preset": "beard", "overrides": {"melanin": 0.8}}                        // -> "Beard"
groom_create {"entity": "Hero", "preset": "eyebrows"}                                                    // -> "Eyebrows"
groom_info   {"entity": "Hero"}   // grooms: [{name, strands, attach: {mode: skinned, boundRoots, maskBone}, gpu}]
```

**Level of detail keeps the density.** In real time the strand count of each groom follows its
size on screen (`lodBias` scales it), and all grooms in view share one budget of 600k drawn
strands: every groom keeps a floor of 1500 and the rest is shared in proportion to screen size,
so a crowd of twenty heads costs a bounded amount and the nearest keep their detail. The strands
the LOD drops hand their area to the ones drawn (wider strands, or more coverage below a pixel),
so short hair still reads as a solid volume at 10 m with a few thousand strands. Stills and
movie frames draw every strand (up to 3M over all grooms). `perf_stats {characters: true}` and
`groom_info` report `drawn` and `strands` per groom.

Fur on a beast: `fur_dense` on the body mesh (`strands` 200k–400k, `length` 0.03–0.06, `width`
0.08–0.12 mm), a second groom for the mane (`maskBone` the neck, longer, less `follow`) and the
tail (`maskBone` the first tail bone); a dark `cloth` material under the fur hides gaps.

## Material models (`skin`, `eye`, `cloth`, `hair_card`)

Start from the preset with `material_create {"path": ..., "preset": "skin"}` (or `eye`,
`cloth`, `hair_card`) and set the textures; the fields below tune each model.

| Model | Fields | What it does |
|---|---|---|
| `skin` | `scatterColor`, `scatterRadius` (mm), `lobeMix`, `lobeRoughness`, `microNormal`, `microNormalTiling`, `transmission` | Pre-integrated subsurface scattering (curvature from the geometry, per-channel softened normals, colored shadow edges), two specular lobes (oily + broad), procedural pores, light through ears and nostrils from the sun's shadow-map thickness |
| `eye` | `irisCenter`, `irisCenter2`, `irisRadius`, `irisDepth`, `corneaRoughness`, `eyeShadow`, `limbusDarkening` | Iris refracted under the cornea (parallax), wet cornea highlight, darkened limbus, shadowed corners; `irisCenter2` for atlases with both eyes |
| `cloth` | `sheenColor`, `sheenRoughness`, `fuzz` | Sheen at grazing angles (velvet, wool, cotton), soft fiber rim |
| `hair_card` | `hairShift`, `hairSpecular`, `hairDirection`, `alphaMode` | Anisotropic two-highlight strand shading on cards; `dither` (stochastic, resolves under TAA) or `coverage` (alpha to coverage) |

Typical values: skin `scatterRadius` 1.5–2.5 mm (thin skin and children higher), `microNormal`
0.15–0.35; eyes `irisDepth` 0.03–0.08; cloth `sheenRoughness` 0.3 satin .. 1 felt.

## Debug views

`viewport_capture {"debug_view": ...}` (or `viewport_debug_view`):

| View | Shows |
|---|---|
| `skeleton` | Every animated skeleton over the final image: bones cyan, hips red, hands green, feet orange, head yellow |
| `ik_targets` | Ground probes yellow, foot targets magenta (green when locked in contact), animated ankles gray, hand targets cyan with a red line from the hand, `ik` effectors white |
| `groom_roots` | Groom guide roots: green on the skinned surface, blue rigid, red where an imported root is far from its surface |
| `sss_mask` | Material models: skin red (brighter = more scattering), eye blue, cloth green, hair cards yellow |

## Performance

`perf_stats {"frames": 30, "characters": true, "passes": true}` adds a `characters` section:
animators, `characterIk` components, skinned meshes and vertices, `skinningGpuMs`,
`groomSimGpuMs` (guide simulation + strand rebuild), `groomShadowGpuMs` (deep opacity maps),
`strandsDrawn` / `strandsTotal` and `groomRenderGpuMs` (measured by benchmarking again with
grooms hidden).

## Recipes

**A pack of clips on a different character**

1. `character_inspect {"entity": "Knight"}` → check `humanoid.retargetable`.
2. `animation_retarget {"source": "anims/pack.anim", "target": "Knight", "preview": true}` → both
   maps; fix wrong slots with `target_map`.
3. `animation_retarget {"source": "anims/pack.anim", "target": "Knight"}`, or set
   `animator.retargetFrom` and use the clip names directly.
4. `animation_preview {"entity": "Knight", "clip": "Walk", "times": [0, 0.25, 0.5, 0.75]}`.

**Stairs and slopes**

1. `character_ik {"entity": "Hero", "feet": true, "step_height": 0.4}`.
2. Play, walk up the stairs, `viewport_capture {"debug_view": "ik_targets"}`; `character_inspect`
   → `ik.feet[*].offset`, `ik.pelvisOffset`.

**A strafing character that turns in place**

1. `animator_setup {"entity": "Hero", "preset": "directional"}`.
2. Wander: `set_param(self, "moveX", vx)`, `set_param(self, "moveY", vy)`; when idle and the
   camera turns away, `turn_in_place(self, camera_yaw)`.

**Hair and beard on a running character**

1. `groom_create` with `hair_scalp`, `beard`, `eyebrows` on the character (three groom entities).
2. `viewport_capture {"debug_view": "groom_roots"}` while running: every root green and on the
   skin; `groom_info` → `attach.boundRoots`.
3. `perf_stats {"frames": 30, "characters": true}` → keep `groomSimGpuMs` + `groomRenderGpuMs`
   inside the frame budget (`strands`, `lodBias`).

## Limits

- Feet and hands only: no full-body IK solver, no spine bending toward far hand targets.
- Grooms bind to the mesh they grow on; strand-strand collisions are not simulated.
- Skin scattering is pre-integrated (per pixel), not a screen-space diffusion pass: very thin
  translucent parts rely on the shadow-map transmission term.
