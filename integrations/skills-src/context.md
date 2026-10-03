## Skywalker game engine (agent guidance)

This project is a game built with **Skywalker**, an engine whose whole surface is MCP tools (server name `skywalker`; about 110 tools: scene, entity, render, wander,
sim, view, history, asset, world, network, dcc, studio). Make the game by calling those tools, not by editing scene JSON by hand.

- **Loop:** `engine_info` (once) > `scene_overview` > act (`batch` for 3+ edits) > `viewport_capture` (look!) > verify (`sim_control step`, `sim_trace`, `playtest_run`) > `scene_save`.
- **Conventions:** meters, +Y up, entities face -Z, rotations are Euler degrees `[pitch, yaw, roll]`, colors `#rrggbb`. Entities are referenced by id or exact name.
- **Always look:** never claim something looks right without a capture; for beauty shots use `samples:16`, `overlays:false`, `annotate:false`. Diagnose with `debug_view`.
- **Ground first:** terrain heights are absolute; `terrain_query` / `raycast` before placing anything or the camera.
- **Behaviors** are Wander code: read `wander_reference` before writing any, `wander_check` before `behavior_set`, verify by stepping the simulation.
- **Assets:** reuse (`asset_list`), generate procedurally, or download only CC0 / CC-BY / MIT assets with `asset_download` (the human approves; give the license exactly as stated).
- **Studio:** a roster of agents, a task board, feedback and playtest bots live in the project (`studio_overview`). Act as a roster member with `as:"<agent id>"`; only the director decides feedback.
- **Skills:** use the `skywalker-*` skills (core, world-building, look-dev, assets, dcc, studio, audio, wander, physics, 2d-ui, animation, vfx). The engine docs are also served as MCP resources (`skywalker://docs/...`) and
  workflow prompts via `prompts/list`.
- If the `skywalker` tools are missing, ask the human to run `skywalker setup <claude|codex|gemini|cursor>` (or check the MCP server list).
