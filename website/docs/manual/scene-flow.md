# Scene flow

Games move between scenes while they run: a menu, levels, a credits roll. The editor's `scene_load` opens a scene for editing; the **scene flow** is what the running game does. `change_scene` replaces the world while entities marked `carry` (a game manager, the player, the music) come along with their behaviors still running, sub-scenes (rooms, streaming chunks, UI overlays) come and go additively, assets preload behind an optional loading scene, and the screen fades or crossfades. Stopping play throws it all away: the editor gets back the scene that was open when play started.

## Changing scenes

```wander
on ui "Play"
  change_scene("level1", {transition: "fade", duration: 0.5, spawn_at: "Spawn"})
end
```

The target is a `game.json` alias (`"scenes": {"level1": "scenes/level1.sky.json"}`) or a path (`scenes/level1.sky.json`, `rooms/cellar`). An unknown name is an error with a *did you mean …?* hint. A change never happens while scripts run; it goes through these phases:

1. **Requested** during a tick, **activated** at its end: every behavior hears `on scene_unloading` on the next tick.
2. **Out**: the screen fades to the transition color (`fade`).
3. **Loading**: an optional loading scene replaces the world while the target's meshes, materials and prefabs preload a few per tick (`loading_progress()` climbs to 1).
4. **Swap** at the end of a tick: carried entities stay, everything else goes, the new scene is cloned in, `spawn_at` moves the player, and `on scene_loaded` follows.
5. **In**: the screen fades back, or the old scene's last frame dissolves over the new one (`crossfade`).

Entities of a scene entered at run time get fresh ids, and ids are never reused in a play session, so a reference a carried entity holds can never land on something in the next scene.

## Carried entities

| How | Use |
|---|---|
| A `carry` component | `{"id": "game_manager", "spawn": false}`: the usual way |
| `game.json` `sceneFlow.carry: ["Music"]` | Entity names, without touching the scenes |
| `keep: ["Companion"]` in one change | Just this once |

Carried entities keep running: behaviors are not restarted, timers and waits continue, vars keep their values. Put the same game manager (same `carry.id`, or the same name) in every level so each level also runs on its own; when the game arrives from another scene, the level's copy gives way to the running one. With `spawn_at`, carried entities with `carry.spawn: true` (or, without any, those tagged `player`) move to the named entity of the new scene.

`carry` is not `persist`: `carry` says what survives a scene change, [`persist`](save-games.md#the-persist-component) says what a save game restores. Loading a save made in another scene is an immediate scene change through the scene flow, and carried entities come along.

## Sub-scenes

```wander
var room = ""
on trigger_enter "player"
  if room == "" then room = load_additive("rooms/hall_2", {offset: (60, 0, 0)}) end
end
on trigger_exit "player"
  if room != "" then
    unload_scene(room)
    room = ""
  end
end
```

A sub-scene's entities join the running scene (fresh ids, optionally under `parent` and moved by `offset`). It owns exactly the entities it created: unloading removes those and nothing else, and entities the game added under them are kept and reported as orphans. A scene change removes every sub-scene.

## Transitions

| Kind | What players see |
|---|---|
| `none` | A cut |
| `fade` | Fade to `color` over `duration`, swap behind it, fade back over `duration` |
| `crossfade` | The old scene's last frame dissolves over the new one over `duration` |

Transitions are state, not effects baked into scenes: `scene_flow_info` reports the phase and opacity, and every frame carries them to the renderer, which draws them over the whole picture, UI included. Captures taken mid-fade are darker; the Metal renderer also crossfades, the CPU renderer cuts.

## game.json

```json
{
  "scenes": {"menu": "scenes/menu.sky.json", "level1": "scenes/level1.sky.json", "credits": "scenes/credits.sky.json"},
  "sceneFlow": {
    "carry": ["Music"],
    "loadingScene": "scenes/loading.sky.json",
    "transition": {"kind": "fade", "duration": 0.4, "color": "#000000"},
    "preloadPerTick": 4
  }
}
```

## Checking a flow as an agent

```tool
sim_control {"action": "play"}
scene_change {"scene": "level1", "transition": "fade", "spawn_at": "Spawn"}
sim_control {"action": "step", "ticks": 60}
scene_flow_info {}
scene_additive_load {"scene": "rooms/cellar", "offset": [40, 0, 0]}
scene_additive_unload {"handle": "cellar"}
sim_control {"action": "stop"}
```

`scene_change` with `"immediate": true` swaps between ticks without a transition. The Wander side has `current_scene()` and `loading_progress()` for menus and loading bars.

## See also

- Tools: [`scene_flow_info`](../reference/tools/scene.md#scene_flow_info), [`scene_change`](../reference/tools/scene.md#scene_change), [`scene_additive_load`](../reference/tools/scene.md#scene_additive_load), [`scene_additive_unload`](../reference/tools/scene.md#scene_additive_unload).
- Component: [`carry`](../reference/components/game.md#carry).
- Related pages: [Save games](save-games.md), [Scenes and entities](scenes.md), [Simulation and time](simulation.md).
- Design document: [docs/SCENE_FLOW.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/SCENE_FLOW.md).
