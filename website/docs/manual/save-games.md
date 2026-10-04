# Save games

Scene files are for authoring, and the editor's play snapshot brings the edited scene back when play stops. **Save games** are what a game itself uses: checkpoints, autosaves, quicksaves and a *Continue* button. A save captures the running game between two ticks, and a load puts it back into the running game exactly, so the game replays from there tick for tick.

## What a save holds

| Part | Notes |
|---|---|
| Persisted entities | Every entity with a `persist` component: its whole state or chosen fields, plus its Wander vars |
| Spawned entities | Entities created at run time with `persist.spawned`: recreated with their children and their saved ids |
| Tombstones | Persisted scene entities destroyed before the save (a collected key stays collected) |
| Game variables | `game_var(name, value)`: state that belongs to the game, not to one entity (chapter, flags, settings) |
| Wander state | Clocks, the random generator, queued events, and per persisted entity its state machines, timers and waiting handlers |
| The scene | The scene the game was in and the ids its entities had; a load returns to it through the [scene flow](scene-flow.md) |
| Play time and metadata | Seconds played in this save line, and a free object for menus (`{title, chapter, thumbnail}`) |

Everything else (scenery, entities without `persist`, particles, animation poses, sounds) keeps its current state when a save is loaded.

## The persist component

```json
"persist": {"id": "player", "mode": "all", "vars": true}
"persist": {"mode": "fields", "fields": ["transform.position", "light.intensity"], "vars": false}
"persist": {"spawned": true}
```

| Field | Default | Meaning |
|---|---|---|
| `id` | `""` | Stable save key. Empty: the entity id (`#12`), stable for entities placed in a scene. Set it when an entity can come back with another id |
| `mode` | `all` | `all`: name, tags, enabled, parent and every component. `fields`: only `fields`. `vars`: Wander vars only |
| `fields` | `[]` | For `mode: fields`: component names (`"transform"`) or `component.field` (`"body.velocity"`) |
| `vars` | `true` | Also save the entity's Wander vars |
| `spawned` | `false` | Created at run time: a load recreates it when it is missing and removes copies spawned after the save |

Put `persist` on the player, doors, chests, collectibles, quest givers and a game manager, and `persist.spawned` on the root of prefabs spawned during play. Behavior code never comes from a save: entities keep the game's current scripts, so a game update fixes bugs in old saves too. Values are written at full precision, so a loaded game does not drift.

`persist` is not `carry`: `persist` says what a save restores, [`carry`](scene-flow.md#carried-entities) says what survives a scene change. A player usually has both.

## Saving and loading from Wander

| Function | Notes |
|---|---|
| `save_game(slot, meta?)` | Saves at the end of this tick; `on saved` follows, `on event "save_failed"` on errors |
| `load_game(slot)` | Loads at the end of this tick; false when the slot is empty. `on loaded` follows |
| `has_save(slot)`, `list_saves()`, `delete_save(slot)` | Slot queries for menus |
| `game_var(name)` / `game_var(name, value)` | Reads or sets a global game variable; saved with every slot, reset when play stops |

A game manager owns the save logic; checkpoints tell it when to save:

```wander
var checkpoint = ""
on start
  find("Continue").ui.visible = has_save("autosave")
end
on event "checkpoint" with cp
  if cp.name != checkpoint then
    checkpoint = cp.name
    game_var("checkpoint", cp.name)
    save_game("autosave", {title: cp.title, chapter: game_var("chapter")})
  end
end
on ui "Continue"
  load_game("autosave")
end
on loaded
  find("Chapter").text.text = "Chapter " + str(game_var("chapter"))
end
```

Saving and loading wait for the end of the tick on purpose: a save in the middle of a tick would capture half of it, and a load would replace the world under the handler that asked for it.

## Testing saves as an agent

Saving and loading need play mode; listing and deleting work while editing. Right after a load, `save_inspect` should report every persisted entity as `same`; anything that `differs` names the fields that did not restore.

```tool
sim_control {"action": "play"}
sim_control {"action": "step", "ticks": 600}
save_game {"slot": "test", "meta": {"title": "Before the bridge"}}
sim_control {"action": "step", "ticks": 300}
load_game {"slot": "test"}
save_inspect {"slot": "test"}
save_list {}
```

## Slots, files and versions

- Slot names are 1 to 64 characters of `a-z 0-9 _ -`; `autosave` and `quicksave` do not count toward `maxSlots`.
- `game.json`: `"saves": {"version": 1, "maxSlots": 20, "compress": false, "migrate": "scripts/save_migrate.wander"}`.
- Files live in `<project>/.skywalker/saves/` while you work; a shipped app keeps them in the player's data folder (`~/Library/Application Support/<game id>/saves` on macOS, `~/.local/share/<game id>/saves` on Linux).
- Writes are atomic (a crash mid-save keeps the previous save), files carry an integrity hash, and `compress: true` writes gzip.
- When the shape of saved data changes, bump `saves.version` and add `fn migrate(from, data)` in the `saves.migrate` file; older saves are migrated one version at a time, and saves from a newer game are refused.

## How a load works

1. If the save was made in another scene, or in the same scene entered with other entity ids, the scene flow changes to it at once, with no transition and no loading scene, and the scene's entities get the ids they had in the save. Carried entities come along and keep running, as on any scene change; a scene change already under way is dropped.
2. Tombstoned entities are destroyed, and so are persisted entities spawned after the save.
3. Each saved entity is restored in place, or recreated with its saved id and children when it was spawned.
4. Entity order, the next entity id, game variables and the exact Wander state are restored; physics is rebuilt from the restored scene.
5. `on loaded` runs on the next tick.

Saving at tick N, loading, and running M more ticks gives the same world as running N + M ticks directly, also when the save is loaded from another scene. Stopping play still returns the editor to the scene as it was edited.

## See also

- Tools: [`save_game`](../reference/tools/sim.md#save_game), [`load_game`](../reference/tools/sim.md#load_game), [`save_list`](../reference/tools/sim.md#save_list), [`save_inspect`](../reference/tools/sim.md#save_inspect), [`save_delete`](../reference/tools/sim.md#save_delete).
- Component: [`persist`](../reference/components/game.md#persist).
- Related pages: [Scene flow](scene-flow.md), [Simulation and time](simulation.md), [Shipping](shipping.md).
- Design document: [docs/SAVE_GAMES.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/SAVE_GAMES.md).
