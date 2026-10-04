# Skywalker tools by task

Arguments marked `*` are required. Entity arguments accept a numeric id or an exact name.
`skywalker tools --markdown` (or the MCP resource `skywalker://tools`) prints the live catalogue with
full descriptions; `component_schema` and each tool's input schema list every field.

## Scene and entities

| Task | Tool and key arguments |
|---|---|
| Orient | `engine_info`, `scene_overview {max_entities}`, `scene_query {name glob, tag, component, near, radius, limit}` |
| Create | `entity_create {name*, mesh, color, position, rotation, scale, tags, vars, parent, components}` |
| Change | `entity_update {entity*, name, enabled, tags, vars, parent, unique, components}` (merge; null removes a component), `transform {entity*, position, rotation, scale, translate, rotate, space:local\|world}` |
| Inspect | `entity_get {entity*}` (all components, vars, behaviors), `component_schema {component}` |
| Copy / remove | `entity_duplicate {entity, entities, name, offset, count}` (a group keeps its internal links), `entity_copy {entities*}` + `entity_paste {data*, parent, offset}`, `entity_delete {entity*}` (with children) |
| References | `entity_refs {entity}` (incoming/outgoing links; without entity: dangling links, name-only links, duplicate unique names, broken prefabs) |
| Many edits | `batch {operations*:[{tool, args}], label}` atomic, one undo step |
| Files | `scene_new {name, empty}`, `scene_save {path}`, `scene_load {path*}` |
| Undo | `history {action*: undo\|redo\|list, steps, limit}` |
| Selection (show the human) | `selection_get`, `selection_set {entities*}` |

## Looking

| Task | Tool |
|---|---|
| Render | `viewport_capture {width, height, view:editor\|scene, camera_entity, eye, target, fov, aperture, focus_distance, annotate, overlays, samples, clay, debug_view, quality:full\|balanced\|fast, include_image, save_path}` |
| Four views | `viewport_multi {focus, size}` |
| Editor camera | `camera_set {frame, eye, target, yaw, pitch, distance}` |
| Pixel to entity | `viewport_pick {x*, y*, width, height}` |
| Editor viewport tier | `viewport_quality {quality: fast\|balanced\|full}` (omit to read; the human's live view, captures stay `full` unless `quality` is passed) |
| Performance | `perf_stats {frames, width, height, view, quality, passes}` (draw calls, lights, GPU/CPU ms; `passes:true` adds `profile.passes` / `groups` / `cpu`) |
| Debug views | `viewport_capture {debug_view}` or live `viewport_debug_view {view, list}`: wireframe, overdraw, unshaded, lighting_only, shadow_cascades, light_complexity, lod, emission, specular, uv_checker, texel_density, albedo, normals, material, gi, reflections, ao, depth, lighting, sketch, impostors, motion, shadow_atlas |
| Light shadows | `shadow_atlas_info {view, entity}` (which lamps have shadows, cache, budgets, casters), `light_shadows {lights, enabled, resolution, mode, max_distance}`, `viewport_capture {debug_view: "shadow_atlas"}` |

## World

| Task | Tool |
|---|---|
| Terrain | `terrain_create {preset*, size, resolution, seed, generator, layers, water, position, name}`, `terrain_generate`, `terrain_sculpt {entity*, strokes*}`, `terrain_paint {entity*, layer*, strokes*}`, `terrain_layers {entity*, layers*, auto_paint}`, `terrain_query {entity, points:[[x,z]]}`, `terrain_undo {entity*}` |
| Foliage | `foliage_add {entity*, layers*, name, area, seed}` |
| Water | `fx_create {effect: ocean\|calm_sea\|storm\|lake\|pool\|puddle ...}`, `water_query {points*}` |
| Place on ground | `raycast {origin, direction, x, y, width, height, exclude}`, `place_on_surface {entities*, offset}`, `scatter {source\|prefab, count*, center, size\|radius, min_distance, yaw, scale, on_surface, surface, seed, group}` |
| Effects | `fx_create {effect*, name, position, parent, overrides}`, `fx_burst {entity*, count}` |
| Reusable sets | `prefab_create {entity*, path*, link}`, `prefab_instantiate {prefab*, position, yaw, rotation, scale, parent, name, on_surface}` (linked instances) |
| Prefab instances | `prefab_overrides {entity, prefab}`, `prefab_revert {entity*, property, all}`, `prefab_apply {entity*, property}`, `prefab_unpack {entity*}`, `prefab_relink {prefab*, entities, max_overrides, dry_run}` |

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

`wander_reference {topic}`, `wander_check {source*, format, disassemble}`, `behavior_set {entity*, name*, intent, spec, source, enabled, allow_errors}`, `behavior_remove {entity*, name*}`, `behavior_spec {entity*, name*}`,
`wander_test {entity, name, source, filter, mode:entity\|scene\|isolated, max_seconds}`, `behavior_graph {entity, name, source, palette}` / `behavior_from_graph {graph*, entity, name, allow_errors}`, `wander_inspect {entity*}`,
`sim_control {action*: play\|pause\|stop\|step\|status, ticks}`, `sim_input {press, hold, release, actions, axes, gamepad, mouse, click, event, data, target}`, `sim_trace {entities*, properties*, ticks, every, press, hold, restore}`, `logs {limit}`, `input_map`.
Debugger: `wander_break_set {script, line, condition, hit_count, log, entity, on_error}`, `wander_break_clear {id, script, on_error}`, `wander_break_list`, `wander_debug_state {wait_ms}`, `wander_continue {wait_ms}`, `wander_step {mode:over\|into\|out, wait_ms}`, `wander_pause {wait_ms}`, `wander_stack {vars}`, `wander_eval {expression*, frame}`, `wander_set_var {name*, value*, frame}`. See skywalker-wander.
Native code: `wander_compile_native {entity, force, auto}`, `native_template {name, overwrite}`, `native_build {force, load}`, `native_list`. See skywalker-wander.

## Physics and navigation

`physics_add {entity\|entities, preset*: prop\|static_level\|kinematic_platform\|player_character\|npc_character\|trigger_zone\|debris\|projectile\|remove, overrides}`, `physics_settle {entities, seconds, freeze_others}`,
`physics_query {type*: raycast\|raycast_all\|shapecast\|overlap, origin*, direction, max_distance, shape, radius, height, half_extents, exclude, include_triggers, layers}`, `physics_debug {view: editor\|scene\|top, focus, show_static, include_image}`,
`physics_settings {gravity, substeps, ignorePairs, allowSleep, enabled}`, `nav_build {agent_radius, agent_height, max_climb, max_slope, cell_size, geometry, save, path}`, `nav_path {from*, to*}`, `nav_debug {focus, from, to, size}`. See skywalker-physics.
2D physics (Box2D): `physics2d_add {entity\|entities, preset*: platformer_player\|crate\|ball\|one_way_platform\|tilemap_collision\|static_ground\|sensor_zone\|moving_platform\|remove, overrides}`,
`physics2d_settle {entity\|entities, seconds, freeze_others}`, `physics2d_query {type*: raycast\|raycast_all\|overlap_circle\|overlap_box\|point, origin*, direction, max_distance, radius, half_extents, angle, exclude, include_sensors, layers}`,
`physics2d_info {entity, limit}`. See skywalker-2d-ui.

## 2D, UI and dialogue

`sprite_sheet_slice {image*, cell, columns, rows, names, animations, entity}`, `sprite_atlas_pack {folder, inputs, output, padding, trim, extrude, fps}`, `tilemap_from_ascii {map*, legend*, entity, name, tileset, tile_size, autotile, solid, solid_tiles, position}`,
`tilemap_paint {entity*, action*: set\|fill\|flood\|clear, tile, cells, rect, at, layer}`, `tilemap_inspect {entity*, layer, max_rects}`, `ui_create {root, elements, template, parent, canvas, width, height}`, `ui_inspect {canvas, element, width, height}`,
`ui_interact {element, at, action: click\|set_value\|type\|scroll\|focus, value, text}`, `ui_style {canvas, theme, path, rules, vars, element, style, css, list}`, `dialogue_check {path, source, entity, start}`,
`dialogue_preview {path, source, entity, start, choices, vars}`, `dialogue_control {action*: start\|advance\|choose\|stop\|state, entity, node, choice}`,
`particles2d_create {preset*: rain\|drizzle\|snow\|leaves\|petals\|fireflies\|smoke\|ripples\|dust\|sparkle, name, position, parent, overrides}`, `particles2d_info {entity}`,
`sprite_sheet_import {folder*, output*, normals, downsample, fps, clips, entity, pivot, pixels_per_unit}` (rendered animation frames -> normal-mapped atlases), `game_feel {action*: hit_stop\|shake\|flash\|info}`. See skywalker-2d-ui.

## Animation and sequences

`animation_list {entity, model, bones}`, `animator_setup {entity*, preset, clips, controller, path, library, root_motion}`, `animator_set {entity*, params, trigger, play, fade, look_at, preview, time}`, `animation_preview {entity*, clip, times, params, view, save_path}`,
`bone_attach {entity*, to*, bone*, offset, rotation}`, `bone_ik {character*, bone*, entity, position, pole, weight}`, `sequence_create {path*, duration, entity}`, `sequence_key {sequence*, keys, camera_cuts, events, animations}`,
`sequence_camera_shot {sequence*, shot*, duration*, camera, target, start, ...}`, `sequence_get {sequence*, time}`, `sequence_scrub {sequence*, times, fps, from, to, save_dir}`, `sequence_play {sequence*, action, from}`. See skywalker-animation.

## Effects and hair

`fx_create {effect*, name, position, parent, overrides}`, `fx_burst {entity*, count}`, `fx_stats`, `fx_benchmark {width, height, frames, eye, target, serial}`, `groom_create {entity*, preset, overrides}`, `groom_update {entity*, preset, fields}`,
`groom_info {entity}`, `groom_export {entity*, path*}`. See skywalker-vfx.

## Shipping

`game_settings {operation: get\|set, settings}`, `game_build {out, name, icon, version, bundle_id, scene, release, all_assets, sign, build_native, dry_run}`, `game_run {action: start\|stop\|status, scene, app, capture, frames, seconds, width, height, fullscreen, quality, wait, pid}`. See skywalker-ship.

## Audio

`audio_generate`, `audio_info {path*}`, `audio_play`, `audio_mix`. See skywalker-audio.

## DCC (Blender)

`dcc_list`, `dcc_generate`, `dcc_run_script`, `dcc_convert`, `dcc_export`, `dcc_edit_asset`, `dcc_install_addon`, `dcc_session_start` / `dcc_session_status` / `dcc_session_exec` / `dcc_session_pull_selection` / `dcc_session_send` / `dcc_session_stop`, `dcc_receive`, `dcc_cancel`. See skywalker-dcc.

## Studio

`studio_overview`, `studio_agent_*`, `studio_team_template`, `studio_memory`, `studio_usage_report`, `studio_task_*`, `studio_feedback_*`, `studio_decide`, `studio_loop_*`, `studio_message_send`, `studio_inbox`, `playtest_run`, `playtest_compare`. See skywalker-studio.

## Approvals

Tools with `openWorld` reach outside the project and need the human's approval in MCP clients: `asset_download`
(network), the script-running `dcc_*` tools, and `game_build` / `game_run` (files and processes outside the project). The native-code tools (`native_build`, `wander_compile_native`) are local but run compiled C++ in the engine process, so MCP clients and the in-editor crew ask for them too: review the code first. Everything else is local, undoable and attributed.
