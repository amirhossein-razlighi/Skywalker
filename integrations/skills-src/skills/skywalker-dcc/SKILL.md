---
name: skywalker-dcc
description: Use Blender (and Maya/Houdini/3ds Max adapters) from Skywalker - procedural models with dcc_generate, convert FBX/OBJ/USD packs to glTF, decimate and bake scans, write bpy scripts, or iterate on a mesh in a live Blender window while seeing it in the engine. Use when an asset must be modeled, converted, repaired or optimized.
---

# DCC bridge (Blender first)

Everything funnels into `.glb`, so a model made or fixed in a design app becomes a normal project asset (with provenance in its
`.meta`). Blender is fully supported; Maya (`mayapy`), Houdini (`hython`) and 3ds Max are adapters that are **untested**: say so
if you use them. These tools run code on the human's computer: **the human approves them** (they are `openWorld`).

## First call: what is available

`dcc_list {}` lists installed apps, versions, running jobs, the live session and the procedural recipes. `dcc_session_status {}` answers "is a live Blender session connected, and what is in it?" (version, open file, object counts, selection, or why it is not connected and how to connect). If Blender is missing,
tell the human to install it or set `SKY_BLENDER` / `~/.skywalker/dcc/paths.json`; do not try to fake it.

## Choose the right tool

| Goal | Tool |
|---|---|
| A building, tower, wall, rock, stairs, arch, fence, column, barrel, terrain chunk | `dcc_generate {recipe, params, name, place, ops}` |
| Something custom | `dcc_generate {script}` or `dcc_run_script {script, import, place}` (Blender Python) |
| FBX / OBJ / DAE / USD / 3DS / PLY / STL / ABC / .blend to GLB | `dcc_convert {path (file or folder), recursive, scale, recenter, ops, skip_existing, place}` |
| Assets out of a `.blend` library | `dcc_export {blend, collections, objects, origin, place}` |
| Reduce triangles, smooth, bevel, UV, bake AO on an existing asset | `dcc_edit_asset {asset, ops, script, mode, update_references}` |
| Iterate interactively with state | `dcc_session_start` then `dcc_session_exec` / `dcc_session_pull_selection` |
| Cancel a long job | `dcc_cancel {job}` (id in `dcc_list`) |

## Recipes

```text
dcc_generate {recipe:"tower", params:{height:14, radius:3.2, ruin:0.6, seed:3}, name:"RuinedTower",
              ops:[{op:"shade_smooth", angle:30}], place:{position:[12,0,-4]}}
viewport_capture {annotate:true}                       # look, then tune params and re-run with another name or seed
```

Recipes are deterministic (same seed, same model), in meters, standing on the ground with the origin at the bottom center. Compose a
scene by calling a recipe repeatedly with different seeds. Parameters: building {floors, width, depth, floor_height, windows_per_side,
roof gabled|hip|flat, wall_style plaster|brick|stone|wood, door, chimney}, tower {radius, height, sides, wall_thickness, battlements,
ruin 0-1, door, window_slits, rubble}, wall {length, height, thickness, ruin}, rock {radius, roughness, flatten, detail, moss},
stairs {steps, width, rise, run, landing, style}, arch {width, height, depth, thickness, segments}, fence {length, height,
post_spacing, style picket|rail}, column {radius, height, flutes, base, capital}, barrel {radius, height, staves, hoops},
terrain_chunk {size, resolution, height, roughness, flat_radius}.

Convert a downloaded pack (after `asset_download` with `import:false`):

```text
dcc_convert {path:"downloads/kenney_town_kit", recursive:true, recenter:"bottom", skip_existing:true, place:{position:[0,0,0], spacing:4}}
```

Use `scale:0.01` for centimeter-authored packs the importer did not convert, `axis_forward`/`axis_up` for odd orientations.

Clean up a heavy scan (original untouched, result is `<name>_v2.glb`):

```text
dcc_edit_asset {asset:"downloads/scan/statue.glb", ops:[{op:"merge_by_distance"},{op:"decimate", ratio:0.2},{op:"shade_smooth", angle:35},
                {op:"bake_ao", target:"vertex", samples:24}], update_references:true}
```

Preset `ops` (run in order): decimate {ratio}, bevel {width, segments, angle}, smart_uv {angle, margin}, merge_by_distance {distance},
triangulate, shade_smooth {angle}, recenter {where: bottom|center}, apply_modifiers, join {name}, bake_ao {samples, distance,
target: vertex|texture, size}. `mode:"replace"` overwrites a `.glb` in place (backup in `.skywalker/dcc-backups`) so every use updates live.

Custom model script (runs headless in Blender; the helper `sky` and `B` are already imported):

```text
dcc_run_script {name:"gatehouse", import:true, place:{name:"Gatehouse", position:[0,0,0]}, script:"
import bpy
from skywalker_dcc import blender as B, procedural as P
B.reset_scene()
tower = P.generate('tower', height=9, radius=2.2, ruin=0.2, seed=5)
wall = P.generate('wall', length=6, height=4, thickness=1.0, ruin=0.1)
wall.location = (3.2, 0, 0)
B.export_glb(sky.out_path('gatehouse.glb'))
sky.result(**B.stats())"}
```

Script environment: `SKY_PROJECT`, `SKY_OUT` (new/changed files here are reported and importable), `sky.out_path(name)`,
`sky.project_path(rel)`, `sky.log()`, `sky.result(**kv)`. Units are meters; Blender is Z-up, glTF export converts to the engine's Y-up.
Write outputs only through `sky.out_path` or they are not imported. Helpers in `B`: reset_scene, import_file, export_glb, stats,
decimate, bevel, merge_by_distance, triangulate, shade_smooth, smart_uv, apply_modifiers, join, recenter, bake_ao, make_material, apply_ops.

## Live Blender session (iterate while watching the engine)

```text
dcc_install_addon {}                                               # once; human clicks N panel > Skywalker > Start Bridge
dcc_session_status {}                                              # connected? version, open file, selection; not connected: says how to connect
dcc_session_start {headless:false, open:"props/lantern.glb"}       # or headless:true for a windowless session
dcc_session_exec {code:"bpy.ops.object.modifier_add(type='SUBSURF')"}        # state persists like a REPL, short calls only
dcc_session_pull_selection {name:"Lantern", origin:"bottom_center"}  # first call imports and places
dcc_session_exec {code:"bpy.context.object.modifiers['Subdivision'].levels = 2"}
dcc_session_pull_selection {name:"Lantern"}                        # same name again: updates the asset in place, entities refresh live
dcc_session_stop {}
```

`dcc_session_send {asset}` pushes a project asset into the session. The human can also click Send Selection in Blender.

## Quality bar for generated geometry

- Look before you move on: `asset_preview` and a capture beside a 1.8 m reference.
- Budget triangles: props 1-10k, hero assets up to 50k, scans must be decimated. Check `stats` in your script's result.
- Pivot at the feet (`recenter:"bottom"`), real-world scale, faces -Z for things with a front.
- Use `shade_smooth` with an angle (30-40) rather than smooth-everything; `bake_ao` adds grounding for free.

## Pitfalls

- Everything in the Blender scene is exported: call `B.reset_scene()` first in scripts or you export leftovers.
- `no_session` errors: start a session or install/start the add-on. `timeout` on session calls: Blender is busy or has a modal dialog.
- 100x too big or small: `scale` on `dcc_convert`, or `normalize:true` on import.
- Blender 3.5 has no 3DS importer (3.6+ does). Windows process execution is not implemented yet.
- Do not run untrusted scripts; the human reviews approvals, give them a one-line description of what the script does.
