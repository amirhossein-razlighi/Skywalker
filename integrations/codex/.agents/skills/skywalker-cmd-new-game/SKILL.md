---
name: skywalker-cmd-new-game
description: "Workflow: Start a new Skywalker game from a one-line pitch - vision, blockout, look, instrumentation, first playtest. Invoke explicitly."
---

Build the opening vertical slice of a new Skywalker game from this pitch: **the request the user wrote after invoking this skill**

Use the `skywalker` MCP tools. Load the `skywalker-core` skill first, then the others you need. Work in this order and stop to show the human after step 4.

1. **Vision (2-3 lines).** Genre, camera, core loop, win/lose, art direction. Write it to the studio as a pinned note: `studio_memory {action:"note", text}` (if a roster exists) and `studio_message_send` in #general.
2. **Orient.** `engine_info`, `scene_overview`. Start from `scene_new {empty:true}` unless the project already has a scene the human wants to keep (ask).
3. **Blockout (gameplay first).** Ground or terrain (`terrain_create` if outdoors; query heights), the player (tag `player`) with a camera, 1 goal (tag `goal`), 1-2 hazards (tag `hazard`).
   Build in one `batch`, verify with `viewport_multi`. Add the player's behavior in Wander (read `wander_reference` first; `wander_check`, `behavior_set`) using input actions, and emit `death` / `objective` events.
4. **Look.** Pick a lighting recipe from `skywalker-look-dev` that fits the vision; capture a beauty shot (`samples:16`, `overlays:false`, `annotate:false`) and show it.
5. **Audio.** A footstep set, one feedback sound, an ambience loop, a music theme (`audio_generate`; verify with `audio_info`).
6. **Prove it plays.** `sim_input` + `sim_control step` + `sim_trace` for the core move, then `playtest_run {policy:"goal_seeker", runs:3}`. Fix what the report shows.
7. **Save** with `scene_save {path:"scenes/main.sky.json"}` and report: what exists, how to play it, the playtest metrics, and the three best next steps.

If the studio is wanted (the pitch is large, or the human asked for a team), run the `studio-setup` command first and assign steps to the roster members instead of doing everything yourself.
