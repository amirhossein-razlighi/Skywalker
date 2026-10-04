# Characters

Skywalker has the pieces a third-person character needs to look and move like a person (or a furred beast): humanoid bone maps that work on any rig, pose-space retargeting of clips between rigs, root motion with turning, automatic foot planting and hand targets, directional blend spaces with turn in place, hair and fur that ride the animated skin, and dedicated skin, eye, cloth and hair-card materials. Skeletal animation basics (libraries, animators, controllers, attachments) are in [Animation](animation.md); strand hair itself is in [Hair and fur](hair.md).

## Start with `character_inspect`

One call reports everything about a character: the humanoid bone map (slot to bone, convention `mixamo` / `suffixed` / `generic`, confidence, missing slots), the world position of key bones, every clip with its root speed and root turn, the root-motion settings, how `retargetFrom` clips map, the last foot and hand IK solve, every mesh part with its material model and every groom with its skin attachment.

```tool
character_inspect {"entity": "Hero"}
```

## Humanoid maps and retargeting

The humanoid map finds slots (hips, spine, chest, neck, head, shoulders, arms, hands, legs, feet, toes) by name *and* by topology, so `mixamorig:LeftUpLeg`, `thigh_l`, `L_Thigh` and `upperleg.L` all land in the same slot, and sideless names are split by their rest position. Fix a wrong guess with `source_map` / `target_map` overrides.

Retargeting works in pose space: each bone's rotation relative to its rest pose is carried over and re-applied on the target's rest pose, so different bone names, bone axes and rest poses (T-pose vs A-pose) line up. The hips move by the source motion scaled by the leg-length ratio, so a short character does not slide, and bones keep their own lengths.

```tool
animation_retarget {"source": "anims/pack.anim", "target": "Knight", "preview": true}
animation_retarget {"source": "anims/pack.anim", "target": "Knight", "clips": ["Walk", "Run", "Idle"]}
```

Or retarget on the fly: set `animator.retargetFrom` to the pack, and clips the character does not have come from it.

## Root motion, turning and in place

| Animator field | Meaning |
|---|---|
| `rootMotion` | Move the entity by the clip's root (hips) motion |
| `rootYaw` | Also turn the entity by the root's rotation about up (turn clips, curved walks) |
| `inPlace` | Remove the root's horizontal motion and yaw: locomotion plays on the spot |

`animator_setup` with the `directional` preset builds a 2D blend space driven by `moveX` and `moveY` (m/s) at the clips' measured root velocities. Direction and speed blend separately, so strafing keeps its speed and the feet do not slide.

## Foot and hand IK

The `characterIk` component plants the feet on the ground under them (stairs, slopes, rocks), lowers the pelvis so the lower foot can reach, tilts the feet to the slope, and locks a foot in contact where it landed until the animation pulls it away. Hands reach entities: a grip point on a staff, a ledge, a rail. `turnSpeed` drives turn in place.

```tool
character_ik {"entity": "Monk", "feet": true, "step_height": 0.4}
character_ik {"entity": "Monk", "left_hand": "Staff Grip"}
```

From Wander: `foot_ik(self, on, weight?)`, `hand_ik(self, "left", find("Staff Grip"), 1)`, `turn_in_place(self, yaw_or_point)` and `look_at(self, target, weight?)`.

## Hair and fur on animated characters

On a rigged mesh a groom's roots are bound to triangles of the posed skin, so strands stay rooted while the character runs, and they collide with capsules fitted to the skeleton (head, neck, chest, shoulders, arms). The presets `hair_scalp`, `beard`, `eyebrows` and `fur_dense` are made for rigged characters.

```tool
groom_create {"entity": "Hero Body", "preset": "hair_scalp", "overrides": {"melanin": 0.85, "length": 0.12}}
groom_create {"entity": "Hero Body", "preset": "beard"}
```

## Material models

| Model | What it does |
|---|---|
| `skin` | Pre-integrated subsurface scattering from the surface curvature, two specular lobes, procedural pores, light through ears and nostrils |
| `eye` | Iris refracted under the cornea, wet cornea highlight, darkened limbus, shadowed corners |
| `cloth` | Sheen at grazing angles (velvet, wool, cotton) and a soft fiber rim |
| `hair_card` | Anisotropic two-highlight strand shading on cards, dithered or alpha-to-coverage transparency |

Start from a preset with `material_create {"path": "materials/face.mat.json", "preset": "skin"}` and set the textures. All four models also take the reflection probes' light, like every other surface.

## Debug views and performance

| View | Shows |
|---|---|
| `skeleton` | Every animated skeleton over the final image |
| `ik_targets` | Ground probes, foot targets (green when locked), hand targets |
| `groom_roots` | Groom roots: green on the skinned surface, blue rigid, red where far from their surface |
| `sss_mask` | Material models: skin red, eye blue, cloth green, hair cards yellow |

`perf_stats {"frames": 30, "characters": true}` adds skinning, groom simulation and groom rendering cost.

!!! agent "For agents"

    Inspect first, then change one thing and look:

    ```tool
    character_inspect {"entity": "Knight"}                                                         # is the humanoid map retargetable?
    animation_retarget {"source": "anims/pack.anim", "target": "Knight", "preview": true}          # both maps, before writing
    character_ik {"entity": "Knight", "feet": true}                                                 # plant the feet
    viewport_capture {"debug_view": "ik_targets"}                                                    # see the targets
    ```

## Reference

- Tools: [`character_inspect`](../reference/tools/animation.md#character_inspect), [`character_ik`](../reference/tools/animation.md#character_ik), [`animation_retarget`](../reference/tools/animation.md#animation_retarget), [`groom_create`](../reference/tools/render.md#groom_create).
- Component: [`characterIk`](../reference/components/animation.md#characterIk).
- Related pages: [Animation](animation.md), [Hair and fur](hair.md), [Materials](rendering/materials.md), [Debug views](rendering/debug-views.md).
- Design document: [docs/CHARACTERS.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/CHARACTERS.md).
