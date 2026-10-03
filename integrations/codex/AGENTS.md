# Skywalker game project

<!-- BEGIN SKYWALKER (managed by `skywalker setup`; edit outside these markers) -->
## Skywalker game engine (agent guidance)

This project is a game built with **Skywalker**, an engine whose whole surface is MCP tools (server name `skywalker`; about 160 tools: scene, entity, view, render, sim, wander, code,
physics, animation, ui, dialogue, asset, world, network, dcc, studio, files, history). Make the game by calling those tools, not by editing scene JSON by hand.

- **Loop:** `engine_info` (once) > `scene_overview` > act (`batch` for 3+ edits) > `viewport_capture` (look!) > verify (`sim_control step`, `sim_trace`, `playtest_run`) > `scene_save`.
- **Conventions:** meters, +Y up, entities face -Z, rotations are Euler degrees `[pitch, yaw, roll]`, colors `#rrggbb`. Entities are referenced by id or exact name.
- **Always look:** never claim something looks right without a capture; for beauty shots use `samples:16`, `overlays:false`, `annotate:false`. Diagnose with `debug_view`. Captures render at `quality:"full"`; the human's live viewport may be on the `fast` tier (`viewport_quality`).
- **Ground first:** terrain heights are absolute; `terrain_query` / `raycast` before placing anything or the camera.
- **Behaviors** are Wander code: read `wander_reference` before writing any, `wander_check` before `behavior_set`, verify by stepping the simulation.
- **Assets:** reuse (`asset_list`), generate procedurally, or download only CC0 / CC-BY / MIT assets with `asset_download` (the human approves; give the license exactly as stated).
- **Studio:** a roster of agents, a task board, feedback and playtest bots live in the project (`studio_overview`). Act as a roster member with `as:"<agent id>"`; only the director decides feedback.
- **Skills:** load the specialist `skywalker-*` skill before working in its area (core is the router): world-building (terrain, foliage, water), look-dev (lighting, post, viewport quality), assets, dcc (Blender), audio, wander (behaviors, native code),
  physics (bodies, navmesh), 2d-ui (sprites, tilemaps, UI, dialogue), animation (animators, IK, cutscenes), vfx (particles, hair), ship (macOS app builds), studio (crew, playtests). The engine docs are also served as MCP resources (`skywalker://docs/...`) and
  workflow prompts via `prompts/list`.
- If the `skywalker` tools are missing, ask the human to run `skywalker setup <claude|codex|gemini|cursor>` (or check the MCP server list).

Codex specifics: skills are in `.agents/skills` (invoke with `$skywalker-core`, or let Codex pick them from their descriptions); the studio roles are custom agents in `.codex/agents/` (`creative_director`, `level_designer`, `environment_artist`, `gameplay_programmer`, `technical_artist`, `sound_designer`, `playtester`, `critic`): spawn them explicitly and give each its assignment. Workflows are the skills named `skywalker-cmd-*` (`$skywalker-cmd-new-game <pitch>`). MCP tool calls can be slow (captures, playtests, DCC): keep `tool_timeout_sec` generous.
<!-- END SKYWALKER -->
