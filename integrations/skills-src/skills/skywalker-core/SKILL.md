---
name: skywalker-core
description: Use whenever you build, edit, inspect or test a game with the Skywalker engine through its MCP tools (engine_info, scene_overview, entity_create, batch, viewport_capture, history). The core agent loop, conventions and pitfalls, and the router to the specialised skywalker-* skills (world-building, look-dev, assets, dcc, studio, audio, wander, physics, 2d-ui, animation, vfx, ship). Load this first.
---

# Skywalker core: the agent loop

Skywalker is a game engine whose entire surface is MCP tools (about 160, grouped in categories
scene, entity, view, render, sim, wander, code, physics, animation, ui, dialogue, asset, world, network, dcc, studio, files, history). Everything a
human can do in the editor, you can do through tools, and everything is undoable and attributed to you.

If no `skywalker` tools are visible in your tool list, the MCP server is not connected: ask the human to
run `skywalker setup <claude|codex|gemini|cursor>` (or `/mcp` in Claude Code to inspect the server).

## The loop

1. **Orient (once).** `engine_info` (version, renderer, component types, tool categories) then
   `scene_overview` (every entity as one line, environment, selection). Do not call `engine_info` again.
2. **Act.** Prefer `batch` for 3+ edits: atomic, one undo step, and the failing index is reported.
3. **Look.** `viewport_capture` returns a PNG and every visible entity with its screen box. Never claim
   something looks right without having captured it.
4. **Verify.** Behaviors: `sim_control step` + `logs` / `sim_trace`. Layout: `viewport_multi`. Looks: `debug_view`.
   Gameplay: `playtest_run`. Performance: `perf_stats` (`passes:true` for where the frame time goes).
   Shot quality: `scene_audit {view:"scene", strict:true}` (visible primitives, primitive characters, untextured surfaces).
5. **Fix or finish.** Wrong? `history {action:"undo"}` and retry differently. Done? `scene_save`, then report.

```text
engine_info {}                       scene_overview {}
batch {label:"Camp", operations:[
  {tool:"entity_create", args:{name:"Tent", mesh:"cone", color:"#c8a165", position:[0,1,0], scale:[2,2,2]}},
  {tool:"entity_create", args:{name:"Fire", components:{light:{kind:"point", intensity:6, color:"#ff9a4a", range:14}, transform:{position:[3,0.6,1]}}}}]}
viewport_capture {annotate:true}     # ids are drawn on the image
```

## Conventions (violating these is the top source of broken scenes)

- Meters, **+Y up**, entities face **-Z**, rotations are Euler degrees `[pitch(X), yaw(Y), roll(Z)]`.
- Colors are `"#rrggbb"` (or `[r,g,b]` in 0..1). Emissive alpha is strength and may exceed 1 (HDR, glows with bloom).
- Entity references are a numeric id or an **exact** name. Ids are stable; names can collide, so prefer ids after creation.
- `entity_update` merges component fields; set a component to `null` to remove it. `components` take full
  field names (`component_schema {component:"mesh"}` lists every field with ranges and enums).
- A directional light uses the entity's rotation; point and spot lights use position. Environment (sun, sky, fog,
  post) is separate: `environment_update` (see skywalker-look-dev).
- Primitives: cube, sphere, plane, cylinder, cone, quad, capsule, torus (plus grass/rock helpers). Models are
  `asset:<path>` (.glb/.gltf/.obj). Imported meshes are normalized to a 1 m cube unless you say otherwise.
- Edits are **in memory**. In headless mode nothing survives the session until `scene_save {path}`. In the
  attached editor the human sees edits live but still needs the scene saved.

## Efficiency rules

- Minimize round trips: `batch`, `scatter`, `foliage_add`, `prefab_instantiate` instead of loops of single creates.
- Entity-link fields (joint `target`, camera2d `follow`, animator `lookAt`, ...) store ids and survive renames. Before renaming or deleting something others point at, run `entity_refs {entity}`; after big edits, `entity_refs {}` lists dangling links with the fix. Duplicate rigs together (`entity_duplicate {entities}`) so their links stay internal.
- `scene_query` (name glob, tag, component, proximity) beats re-reading `scene_overview` on big scenes.
  `scene_overview {max_entities}` limits output.
- Iterate cheap, finish expensive: `viewport_capture {width:640, height:360, samples:1}` while exploring,
  then `samples:16` and a larger size for the final judgement. `include_image:false` returns only the entity list.
- `viewport_capture` and `perf_stats` take `quality` (`full` default, `balanced`, `fast`): captures are `full` unless you ask otherwise. The human's live editor viewport has its own tier (`viewport_quality`, `fast` by default:
  half-resolution, no GI/reflections/god rays), so it can look flatter than your capture; see skywalker-look-dev.
- Keep captures purposeful: one overview shot, then targeted `camera_entity` / `eye`+`target` shots.
- Read tool errors: they carry a code, a hint and did-you-mean suggestions (`unknown entity 'Tnet' - did you mean 'Tent'`).

## Looking at things

| Need | Tool |
|---|---|
| What is where, and its id | `viewport_capture {annotate:true}` then match `#id` labels and boxes |
| Whole-level layout, alignment, spacing | `viewport_multi` (perspective + top + front + side) |
| What the game camera sees | `viewport_capture {view:"scene"}` or `camera_entity` |
| Frame an entity in the editor view | `camera_set {frame:"Tent"}` or `{frame:"all"}` |
| What is under pixel (x,y) | `viewport_pick`, `raycast {x,y,width,height}` |
| Why does it look wrong | `viewport_capture {debug_view: albedo|normals|material|ao|gi|reflections|depth|lighting|unshaded|lighting_only|specular|emission}` |
| Why is it slow | `perf_stats {frames:30, passes:true}` (GPU ms per pass and per area, CPU scopes), then `debug_view: overdraw|lod|light_complexity|wireframe` |
| Show the human a debug view live | `viewport_debug_view {view:"wireframe"}` (`"final"` turns it off; `{list:true}` gives every view's color legend) |
| Camera under the ground? | `terrain_query` / `raycast` straight down (terrain heights are absolute) |

## Simulation

`sim_control` plays, pauses, `step`s N fixed 1/60 s ticks deterministically, or stops (stop restores the scene to
its pre-play state, so **edits made while playing are discarded**). Drive input with `sim_input` (keys, input
actions, axes, mouse, gamepad, click an entity), then step. `sim_trace` samples properties over N ticks (e.g.
`transform.position`, `vars.score`) so you can verify numerically instead of eyeballing. `logs` shows Wander output
and runtime errors. Behaviors are Wander code: see skywalker-wander.

Two pauses: `sim_control {action:"pause"}` is the editor's (nothing ticks); `{action:"pause_game"}` is the game's own
pause menu pause (`pause_game()` in Wander): `pausable` entities freeze, UI canvases and `process` mode
`always`/`when_paused` entities keep running. `{action:"time_scale", scale:0.25}` is slow motion. Both apply from the
next tick. `process_info {entity}` explains what runs while paused and why (and warns when nothing could resume).
Real-time frames are interpolated between the 60 Hz ticks (smooth on 120 Hz displays); `sim_trace {display_hz:120}`
measures it (`smoothness.stepJitter` near 0 = smooth) and `sim_teleport` moves something without a smear.

## Attribution and the studio

Every call is attributed (`mcp:<client>` in history and the editor activity feed). To work as a roster member
of the project's studio (tasks, feedback, loops) read skywalker-studio; pass `as:"<agent id>"` on studio calls.

## Pitfalls

- **Camera or props under terrain.** Terrain heights are absolute world values (a "rolling_hills" terrain can sit
  at y=19..28). Always `terrain_query` or `raycast` for the ground before placing anything or the camera.
- **Scatter leaves its source behind.** `scatter` copies `source`; delete or hide the original afterwards.
- **Do not paste the same warm tone into sun + fog + grade + sky**: the image turns monochrome orange.
- **Mutating while playing.** Changes during play are restored on stop. Stop, edit, play again.
- **Large outputs.** `entity_get` on a heavy entity and unbounded `scene_overview` cost context; query narrowly.
- **Approvals.** `asset_download` (network), `dcc_*` script runners, `game_build` / `game_run` and the native-code tools need the human's approval. Ask for it up
  front instead of retrying in a loop.
- **Do not hand-write scene JSON** for things tools can do; tool calls validate and are undoable.

## Where to go next

Load the specialist skill **before** the first call in its area; each has the workflow, exact example calls and the pitfalls.

| Task | Skill | Main tools |
|---|---|---|
| Terrain, foliage, water, placing things on ground | skywalker-world-building | `terrain_*`, `foliage_add`, `scatter`, `place_on_surface`, `water_query` |
| Lighting, sky, post, camera lens, judging renders, viewport quality tiers | skywalker-look-dev | `environment_update`, `material_*`, `viewport_capture`, `viewport_quality`, `perf_stats` |
| Models, textures, downloads, licenses | skywalker-assets | `asset_*`, `texture_generate` |
| Blender / DCC round trips, procedural models | skywalker-dcc | `dcc_*` |
| Sound, music, input maps | skywalker-audio | `audio_*`, `input_map` |
| Gameplay scripts, AI, game rules, native speedups | skywalker-wander | `wander_*`, `behavior_*`, `native_*` |
| Rigid bodies, characters, triggers, joints, navmesh pathfinding | skywalker-physics | `physics_*`, `nav_*` |
| Sprites, tilemaps, 2D lights, HUD/menus/UI, dialogue | skywalker-2d-ui | `sprite_*`, `tilemap_*`, `ui_*`, `dialogue_*` |
| Characters, state machines, IK, props on bones, cutscenes and cameras | skywalker-animation | `animation_*`, `animator_*`, `bone_*`, `sequence_*` |
| Fire, smoke, weather, GPU particles, hair and fur, effect cost | skywalker-vfx | `fx_*`, `groom_*` |
| Packaging a macOS app, testing as a player, release settings | skywalker-ship | `game_settings`, `game_build`, `game_run` |
| Team of agents, tasks, feedback, playtests, loops, token usage | skywalker-studio | `studio_*`, `playtest_*` |
| No tool does what you need: define your own (Wander, composite, hosted) | skywalker-custom-tools | `tool_define`, `tool_test`, `tool_inspect` |

Full tool catalogue by task: [references/tools-by-task.md](references/tools-by-task.md). The server also exposes the
engine docs as MCP resources (`skywalker://docs/...`) and the tool catalogue (`skywalker://tools`).
