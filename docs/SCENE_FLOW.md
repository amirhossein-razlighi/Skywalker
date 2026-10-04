# Scene flow

Games move between scenes while they run: a menu, levels, a credits roll. The editor's `scene_load`
opens a scene for editing; **scene flow** is what the running game does: `change_scene` replaces the
world while entities marked `carry` (a game manager, the player, the music) are carried over, sub-scenes
(rooms, streaming chunks, UI overlays) come and go additively, assets preload behind a loading screen,
and the screen fades or crossfades. Stopping play throws it all away: the editor gets back the scene
that was open when play started.

- [Changing scenes](#changing-scenes)
- [Carried entities](#carried-entities)
- [Sub-scenes (additive)](#sub-scenes-additive)
- [Loading and progress](#loading-and-progress)
- [Transitions](#transitions)
- [game.json](#gamejson)
- [Wander](#wander)
- [Tools](#tools)
- [Recipes](#recipes)
- [Guarantees and limits](#guarantees-and-limits)

## Changing scenes

```text
change_scene("level2", {transition: "fade", duration: 0.5, spawn_at: "Door_West"})
```

The scene is a game.json alias (`"scenes": {"level2": "scenes/level2.sky.json"}`) or a path:
`scenes/level2.sky.json`, `level2` (looked up in `scenes/`), `rooms/cellar` (`.sky.json` added). An
unknown name is an error with a did-you-mean.

A change never happens while scripts run. It goes:

1. **Requested** (this tick).
2. **Activated** at the end of the tick: every behavior hears `on scene_unloading` on the next tick,
   the leaving scene included.
3. **Out**: the screen fades to the transition color (`fade`), or nothing happens (`none`, `crossfade`).
4. **Loading**: an optional loading scene replaces the world; the target's assets preload a few per tick
   (`loading_progress()` climbs to 1).
5. **Swap** (end of a tick): carried entities stay, everything else is removed, the new scene is
   cloned in, `spawn_at` moves the player, `on scene_loaded` follows on the next tick.
6. **In**: the screen fades back (or crossfades from the old picture).

New scene entities get fresh entity ids. Ids are never reused during a play session, so a reference a
carried entity holds (`target = find("Boss")`) can never land on something in the next scene: it
simply stops existing.

## Carried entities

Three ways to carry an entity (and its children) over:

| How | Use |
|---|---|
| A `carry` component | `{"id": "game_manager", "spawn": false}`: the usual way |
| game.json `sceneFlow.carry: ["Music"]` | Entity names, without touching the scenes |
| `keep: ["Companion"]` in a change | Just this once |

Carried entities keep running: their behaviors are not restarted, timers and waits continue, vars
keep their values. Put the same game manager (same `carry.id`, or the same name) in every level so
each level also runs on its own; when the game arrives from another scene, the level's copy gives way to
the running one.

`spawn_at` names an entity of the new scene; carried entities with `carry.spawn: true` (or,
without any, those tagged `player`) move to its position and rotation, without interpolation smear.

A carried child of a parent that is not carried becomes a root entity (keeping its local transform).
Carried entities should normally be roots.

## Sub-scenes (additive)

```text
let cellar = load_additive("rooms/cellar", {offset: (40, 0, 0)})
...
unload_scene(cellar)
```

A sub-scene's entities join the running scene (fresh ids, optionally under `parent` and moved by
`offset`); the scene's environment is not touched. The sub-scene **owns exactly the entities it
created**: unloading removes those and nothing else. Entities the game added under them at run time
(loot dropped in a room) are kept and moved to the root; the unload result lists them as `orphans`.
Handles default to the alias or file name and are made unique (`cellar`, `cellar#2`). A scene change
removes every sub-scene (carried entities inside one are kept).

From Wander, loads happen at the end of the tick and unloads one tick after `on scene_unloading` was
heard; the tools load and unload immediately (between ticks).

## Loading and progress

The target scene's assets (meshes, materials, prefabs) preload during the **Loading** phase,
`sceneFlow.preloadPerTick` per tick (default 4). Meshes go through the engine's mesh streaming: in a
real-time window they load on background threads and the loading phase waits for them; headless (tests,
`sim_control step`, tools) they load at once, so a run replays tick for tick.

A **loading scene** (`sceneFlow.loadingScene`, or `loading` in a change) replaces the world for the
loading phase, with the carried entities still there; read `loading_progress()` in it to fill a bar.

```text
-- scenes/loading.sky.json: a ui_canvas with a progress widget named "Bar"
on tick
  find("Bar").ui.value = loading_progress()
end
```

## Transitions

| Kind | What players see |
|---|---|
| `none` | A cut |
| `fade` | Fade to `color` over `duration`, swap behind it, fade back over `duration` |
| `crossfade` | The old scene's last frame dissolves over the new one over `duration` |

Transitions are state, not effects baked into scenes: `scene_flow_info` reports `phase` (`idle`, `out`,
`loading`, `in`) and `alpha`, and every frame carries `FrameData::fade` (`color`, `alpha`, `crossfade`).
The CPU renderer applies fades over the whole picture (UI included), so headless captures show them; GPU
backends draw the same overlay, and a crossfade blends the last frame of the old scene, which the backend
keeps. They advance on the fixed tick (real seconds, not affected by `time_scale`).

## game.json

```json
{
  "startScene": "scenes/menu.sky.json",
  "scenes": {"menu": "scenes/menu.sky.json", "level1": "scenes/level1.sky.json", "credits": "scenes/credits.sky.json"},
  "sceneFlow": {
    "carry": ["Music"],
    "loadingScene": "scenes/loading.sky.json",
    "transition": {"kind": "fade", "duration": 0.4, "color": "#000000"},
    "preloadPerTick": 4
  }
}
```

`startScene` is where the player begins; `scenes` names scenes for scripts and tools; `sceneFlow` sets
defaults that each change can override.

## Wander

| Function | Notes |
|---|---|
| `change_scene(scene, options?)` | Options: `transition` (`"none"`, `"fade"`, `"crossfade"` or `{kind, duration, color}`), `duration`, `color`, `keep: [names]`, `spawn_at: "Name"`, `loading: "scene"` (`""` = none) |
| `load_additive(scene, options?)` | Returns a handle. Options: `id`, `parent` (entity), `offset` (vector) |
| `unload_scene(handle)` | Removes a sub-scene |
| `current_scene()` | The alias of the current scene, else its path |
| `loading_progress()` | 0..1 while preloading, 1 otherwise |

| Trigger | `data` |
|---|---|
| `on scene_unloading` | `{id, to, additive}`: the scene (or sub-scene handle) that goes, and the next scene |
| `on scene_loaded` | `{id, path, additive}`: delivered on the first tick of the new scene, after `on start` |

## Tools

| Tool | Use |
|---|---|
| `scene_flow_info {}` | Current scene, pending change, progress, transition phase and alpha, sub-scenes, carried entities, aliases, scene files |
| `scene_change {scene, transition?, duration?, keep?, spawn_at?, loading?, immediate?}` | Change scene while playing; it runs over the next ticks (`sim_control step`), or now with `immediate: true` |
| `scene_additive_load {scene, id?, parent?, offset?}` | Load a sub-scene now; returns its handle and entity count |
| `scene_additive_unload {handle}` | Remove exactly what it loaded; returns `removed` and `orphans` |

## Recipes

**Menu → level → credits.** A `GameManager` with `carry: {id: "gm"}` in every scene:

```text
-- on the Play button's canvas in the menu
on ui "Play"
  change_scene("level1", {transition: "fade", duration: 0.5})
end
-- on the level's goal trigger
on trigger_enter "player"
  change_scene("credits", {transition: "crossfade", duration: 1.2})
end
```

**Doors between areas.** Each door knows where it leads and where you arrive:

```text
param destination = "forest"
param arrive = "Door_From_Village"
on trigger_enter "player"
  change_scene(destination, {transition: "fade", duration: 0.3, spawn_at: arrive})
end
```

**Streaming rooms.** Load the next room when the player nears it, unload the one behind:

```text
var loaded = ""
on trigger_enter "player"
  if loaded == "" then loaded = load_additive("rooms/hall_2", {offset: (60, 0, 0)}) end
end
on trigger_exit "player"
  if loaded != "" then
    unload_scene(loaded)
    loaded = ""
  end
end
```

**Checking a flow as an agent:**

```text
sim_control {"action": "play"}
scene_change {"scene": "level1", "transition": "fade"}
sim_control {"action": "step", "ticks": 60}
scene_flow_info {}                    -- current: level1, phase idle, carried: GameManager, Player
viewport_capture {}
sim_control {"action": "stop"}       -- back to the edited scene
```

## Guarantees and limits

- Deterministic: changes apply at tick boundaries in a fixed order, and headless preloading takes a fixed
  number of ticks, so the same inputs replay the same flow (tests/test_scene_flow.cpp).
- Stop restores the edited scene exactly; scene changes, sub-scenes and carried state never leak into the
  editor.
- Runtime ids differ from the ids in the scene file (scene files keep theirs; the running copy gets fresh ones).
- Particles, animation and audio of entities that leave stop with them; carried entities' physics bodies are
  rebuilt (from their transform and `body.velocity`) at the swap.
- Save games ([SAVE_GAMES](SAVE_GAMES.md)) record the current scene and its runtime ids. Loading a save made in
  another scene is an immediate change through the scene flow (no transition, no loading scene): carried
  entities come along and keep running, the scene's entities get their saved ids back, and a change under way is
  dropped. `persist` (what a save restores) and `carry` (what survives a change) are separate; a player
  usually has both.
