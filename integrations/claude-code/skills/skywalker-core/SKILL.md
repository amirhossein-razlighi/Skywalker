---
name: skywalker-core
description: "Use whenever you build, edit, inspect or test a game with the Skywalker engine through its MCP tools (engine_info, scene_overview, entity_create, batch, viewport_capture, history). The core agent loop, conventions and pitfalls. Load this first, then the specialised skywalker-* skills."
---

# Skywalker core: the agent loop

Skywalker is a game engine whose entire surface is MCP tools (about 110, grouped in categories
scene, entity, render, wander, sim, view, history, asset, world, network, dcc, studio). Everything a
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
   Gameplay: `playtest_run`. Performance: `perf_stats`.
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
- `scene_query` (name glob, tag, component, proximity) beats re-reading `scene_overview` on big scenes.
  `scene_overview {max_entities}` limits output.
- Iterate cheap, finish expensive: `viewport_capture {width:640, height:360, samples:1}` while exploring,
  then `samples:16` and a larger size for the final judgement. `include_image:false` returns only the entity list.
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
| Why does it look wrong | `viewport_capture {debug_view: albedo|normals|material|ao|gi|reflections|depth|lighting}` |
| Camera under the ground? | `terrain_query` / `raycast` straight down (terrain heights are absolute) |

## Simulation

`sim_control` plays, pauses, `step`s N fixed 1/60 s ticks deterministically, or stops (stop restores the scene to
its pre-play state, so **edits made while playing are discarded**). Drive input with `sim_input` (keys, input
actions, axes, mouse, gamepad, click an entity), then step. `sim_trace` samples properties over N ticks (e.g.
`transform.position`, `vars.score`) so you can verify numerically instead of eyeballing. `logs` shows Wander output
and runtime errors. Behaviors are Wander code: see skywalker-wander.

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
- **Approvals.** `asset_download` (network) and `dcc_*` script runners need the human's approval. Ask for it up
  front instead of retrying in a loop.
- **Do not hand-write scene JSON** for things tools can do; tool calls validate and are undoable.

## Where to go next

| Task | Skill |
|---|---|
| Terrain, foliage, water, placing things on ground | skywalker-world-building |
| Lighting, sky, post, camera lens, judging renders | skywalker-look-dev |
| Models, textures, downloads, licenses | skywalker-assets |
| Blender / DCC round trips, procedural models | skywalker-dcc |
| Team of agents, tasks, feedback, playtests, loops | skywalker-studio |
| Sound, music, input maps | skywalker-audio |
| Gameplay scripts | skywalker-wander |
| Physics, 2D/UI, animation, hair/VFX | skywalker-physics, skywalker-2d-ui, skywalker-animation, skywalker-vfx |

Full tool catalogue by task: [references/tools-by-task.md](references/tools-by-task.md). The server also exposes the
engine docs as MCP resources (`skywalker://docs/...`) and the tool catalogue (`skywalker://tools`).
