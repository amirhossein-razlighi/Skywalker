# Skywalker tools by task

Arguments marked `*` are required. Entity arguments accept a numeric id or an exact name.
`skywalker tools --markdown` (or the MCP resource `skywalker://tools`) prints the live catalogue with
full descriptions; `component_schema` and each tool's input schema list every field.

## Scene and entities

| Task | Tool and key arguments |
|---|---|
| Orient | `engine_info`, `scene_overview {max_entities}`, `scene_query {name glob, tag, component, near, radius, limit}` |
| Create | `entity_create {name*, mesh, color, position, rotation, scale, tags, vars, parent, components}` |
| Change | `entity_update {entity*, name, enabled, tags, vars, parent, components}` (merge; null removes a component), `transform {entity*, position, rotation, scale, translate, rotate, space:local\|world}` |
| Inspect | `entity_get {entity*}` (all components, vars, behaviors), `component_schema {component}` |
| Copy / remove | `entity_duplicate {entity*, name, offset, count}`, `entity_delete {entity*}` (with children) |
| Many edits | `batch {operations*:[{tool, args}], label}` atomic, one undo step |
| Files | `scene_new {name, empty}`, `scene_save {path}`, `scene_load {path*}` |
| Undo | `history {action*: undo\|redo\|list, steps, limit}` |
| Selection (show the human) | `selection_get`, `selection_set {entities*}` |

## Looking

| Task | Tool |
|---|---|
| Render | `viewport_capture {width, height, view:editor\|scene, camera_entity, eye, target, fov, aperture, focus_distance, annotate, overlays, samples, debug_view, include_image, save_path}` |
| Four views | `viewport_multi {focus, size}` |
| Editor camera | `camera_set {frame, eye, target, yaw, pitch, distance}` |
| Pixel to entity | `viewport_pick {x*, y*, width, height}` |
| Performance | `perf_stats` (draw calls, lights, frame build ms) |

## World

| Task | Tool |
|---|---|
| Terrain | `terrain_create {preset*, size, resolution, seed, generator, layers, water, position, name}`, `terrain_generate`, `terrain_sculpt {entity*, strokes*}`, `terrain_paint {entity*, layer*, strokes*}`, `terrain_layers {entity*, layers*, auto_paint}`, `terrain_query {entity, points:[[x,z]]}`, `terrain_undo {entity*}` |
| Foliage | `foliage_add {entity*, layers*, name, area, seed}` |
| Water | `fx_create {effect: ocean\|calm_sea\|storm\|lake\|pool\|puddle ...}`, `water_query {points*}` |
| Place on ground | `raycast {origin, direction, x, y, width, height, exclude}`, `place_on_surface {entities*, offset}`, `scatter {source\|prefab, count*, center, size\|radius, min_distance, yaw, scale, on_surface, surface, seed, group}` |
| Effects | `fx_create {effect*, name, position, parent, overrides}`, `fx_burst {entity*, count}` |
| Reusable sets | `prefab_create {entity*, path*}`, `prefab_instantiate {prefab*, position, yaw, rotation, scale, parent, name, on_surface}` |

## Look and materials

| Task | Tool |
|---|---|
| Sun, sky, fog, post | `environment_get`, `environment_update {preset, sun*, sky*, fog*, gi, ssr, taa, clouds*, godRays, haze, look, lut, tonemap, exposure, ...}` |
| Materials | `material_create {path*, preset, color, metallic, roughness, emissive, texture, normalMap, ormMap, triplanar, ...}`, `material_update {path*, ...}`, `material_assign {entities*, material*}` |
| Procedural textures | `texture_generate {kind*, name*, size, seed, scale, color1..3, create_material, tiling}` |
| Custom shader | `shader_get`, `shader_set` (Metal source; failures return compiler diagnostics) |

## Assets

`asset_list {type, tag, query, limit}`, `asset_info {asset*}`, `asset_preview {asset*, size}`, `asset_import {path*, create_entity, position, normalize, z_up, tags, description}`, `asset_import_mesh {path*, entity}`, `asset_download {url*, license*, author, source_page, attribution, folder, include, import, normalize, z_up, create_entity, position}`, `asset_tag {asset*, tags, description, source}`, `asset_move {asset*, to*}`, `asset_refresh`, generated content queue `asset_request {kind*, prompt*, style, target}` / `asset_requests` / `asset_complete {id*, path}`.

## Behavior and simulation

`wander_reference`, `wander_check {source*}`, `behavior_set {entity*, name*, intent, source, enabled, allow_errors}`, `behavior_remove {entity*, name*}`, `sim_control {action*: play\|pause\|stop\|step\|status, ticks}`, `sim_input {press, hold, release, actions, axes, gamepad, mouse, click, event, target}`, `sim_trace {entities*, properties*, ticks, every, press, hold, restore}`, `logs {limit}`, `input_map`.

## Audio

`audio_generate`, `audio_info {path*}`, `audio_play`, `audio_mix`. See skywalker-audio.

## DCC (Blender)

`dcc_list`, `dcc_generate`, `dcc_run_script`, `dcc_convert`, `dcc_export`, `dcc_edit_asset`, `dcc_install_addon`, `dcc_session_*`, `dcc_receive`, `dcc_cancel`. See skywalker-dcc.

## Studio

`studio_overview`, `studio_agent_*`, `studio_team_template`, `studio_memory`, `studio_task_*`, `studio_feedback_*`, `studio_decide`, `studio_loop_*`, `studio_message_send`, `studio_inbox`, `playtest_run`, `playtest_compare`. See skywalker-studio.

## Approvals

Tools with `openWorld` reach outside the project and need the human's approval in MCP clients: `asset_download`
(network) and the script-running `dcc_*` tools. Everything else is local, undoable and attributed.
