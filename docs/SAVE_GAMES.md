# Save games

Scene files are for authoring, and the editor's play snapshot brings the edited scene back when play stops.
**Save games** are what a game itself uses: checkpoints, autosaves, quicksaves and a "Continue" button. A save
captures the running game between two ticks and a load puts it back, exactly, into the running game.

- [What is saved](#what-is-saved)
- [The persist component](#the-persist-component)
- [Wander](#wander)
- [Tools](#tools)
- [Slots and files](#slots-and-files)
- [Versions and migrations](#versions-and-migrations)
- [How a load works](#how-a-load-works)
- [Recipe: checkpoints and autosave](#recipe-checkpoints-and-autosave)
- [Debugging a load](#debugging-a-load)
- [Limits](#limits)

## What is saved

| Part | Notes |
|---|---|
| Persisted entities | Every entity with a `persist` component: its whole state or chosen fields, plus its Wander vars |
| Spawned entities | Entities created at run time with `persist.spawned`: recreated with their children and their saved ids |
| Tombstones | Persisted scene entities destroyed before the save (a collected key stays collected) |
| Game variables | `game_var(name, value)`: state that belongs to the game, not to one entity (chapter, flags, settings) |
| Wander state | Clocks, the random generator, queued events, and per persisted entity its state machines, timers and waiting handlers |
| The scene | The scene the game was in (its scene flow id: a game.json `scenes` alias or the path) and the ids its entities had; a load switches back to it ([SCENE_FLOW](SCENE_FLOW.md)) |
| Play time | Seconds played in this save line (the loaded save's time plus real time since) |
| Metadata | A free object for menus: `{title, chapter, thumbnail, ...}` |

Everything else (scenery, non-persisted entities, particles, animation poses, sounds) is left as it is in the
running game when a save is loaded.

## The persist component

```json
"persist": {"id": "player", "mode": "all", "vars": true}
"persist": {"mode": "fields", "fields": ["transform.position", "light.intensity"], "vars": false}
"persist": {"spawned": true}
```

| Field | Default | Meaning |
|---|---|---|
| `id` | `""` | Stable save key. Empty: the entity id (`#12`), which is stable for entities placed in a scene. Set it when an entity can be re-created with another id |
| `mode` | `all` | `all`: name, tags, enabled, parent and every component. `fields`: only `fields`. `vars`: Wander vars only |
| `fields` | `[]` | For `mode: fields`: component names (`"transform"`) or `component.field` (`"body.velocity"`) |
| `vars` | `true` | Also save the entity's Wander vars |
| `spawned` | `false` | Created at run time: a load recreates it (with its children) when it is missing, and removes copies spawned after the save |
| `prefab` | `""` | The prefab it came from (filled in from the prefab link; shown by `save_inspect`) |

Put `persist` on the player, doors, chests, collectibles, quest givers and a game manager. Put `persist.spawned`
on the root of prefabs that are spawned during play (enemies, dropped items), so `spawn("prefab:...")` copies
carry it. The behaviors' code is never taken from a save: entities that already exist keep the game's current
scripts, so a game update fixes bugs in old saves too.

Component values are written at full precision (scene files round floats to 4 decimals), so a loaded game does not
drift from the original run.

## Wander

| Function | Notes |
|---|---|
| `save_game(slot, meta?)` | Saves at the end of this tick. `on saved` follows on the next tick; `on event "save_failed"` on errors |
| `load_game(slot)` | Loads at the end of this tick; returns false (and does nothing) when the slot is empty. `on loaded` follows |
| `has_save(slot)` | Whether a slot holds a save |
| `list_saves()` | `[{slot, meta, play_time, saved_at, version, scene}]` sorted by slot; damaged files have `error` |
| `delete_save(slot)` | Deletes a slot; returns whether there was one |
| `game_var(name)` / `game_var(name, value)` | Reads / sets a global game variable (none removes it). Saved with every slot, reset when play stops |

| Trigger | Payload (`data`) |
|---|---|
| `on saved` | `{slot, meta}` |
| `on loaded` | `{slot, version, meta}`: refresh whatever is derived from saved state (HUD text, music) |

Saving and loading wait for the end of the tick on purpose: a save in the middle of a tick would capture half of it,
and a load would replace the scene under the handler that asked for it.

## Tools

| Tool | Use |
|---|---|
| `save_game {slot, meta?}` | Save the running game (play mode), e.g. a checkpoint while playtesting |
| `load_game {slot}` | Restore a slot into the running game; returns counts and warnings |
| `save_list {}` | Slots with metadata, size, format and game version, play time, scene, tick; the folder and settings |
| `save_inspect {slot, max_diffs?}` | Diff a slot against the running game: what would change on load, or what did not restore |
| `save_delete {slot}` | Delete a slot (cannot be undone) |

Saving and loading need play mode (`sim_control {"action": "play"}`); listing and deleting work while editing.

## Slots and files

- Slot names: 1-64 of `a-z 0-9 _ -` (`slot1`, `chapter-2`). `autosave` and `quicksave` are reserved names that do
  not count toward the limit.
- `game.json`: `"saves": {"version": 1, "maxSlots": 20, "compress": false, "migrate": "scripts/save_migrate.wander"}`.
  Saving into a new named slot beyond `maxSlots` fails with `slot_limit`; overwriting is always allowed.
- Where: `<project>/.skywalker/saves/<slot>.save.json` in the editor, tests and a project run from its folder. A
  shipped app uses the user's data folder: `~/Library/Application Support/<game id>/saves` on macOS,
  `$XDG_DATA_HOME/<game id>/saves` (default `~/.local/share/<game id>/saves`) on Linux.
- Writes are atomic: the file is written to a temporary file in the same folder, flushed to disk, then renamed over
  the slot. A crash mid-save leaves the previous save intact.
- `compress: true` writes gzip (`.save.json.gz`, zlib). Loading detects either form.

The file is stable, readable JSON:

```json
{
  "format": "skywalker.save", "formatVersion": 1, "version": 2, "game": "sky-dash", "slot": "autosave",
  "savedAt": "2026-10-04T12:00:00Z", "engine": "0.1.0", "tick": 1834, "playTime": 30.57,
  "scene": "forest", "sceneIds": [[1, 212], [2, 213]], "meta": {"title": "Forest gate", "chapter": 2},
  "globals": {"chapter": 2, "flags": ["met_owl"], "spawn": {"$vec": [3, 0, -4]}},
  "entities": [{"key": "id:player", "id": 4, "name": "Player", "mode": "all", "entity": {...}}],
  "destroyed": ["#17"], "order": [...], "nextId": 212,
  "runtime": {"time": 30.57, "frame": 1834, "rng": {...}, "instances": [...], "vars": [...]},
  "hash": "fnv1a64:9c1f0a2b3d4e5f60"
}
```

- `formatVersion` is the engine's file layout; `version` is the game's own data version.
- `scene` is the scene flow's id of the scene (`scene_flow_info` `current`). `sceneIds` lists `[id in the scene
  file, id in the game]` where they differ: scenes entered with `change_scene` give their entities fresh ids, and a
  load gives them the same ids again so the save's entity ids, behavior state and spawn ids line up.
- `hash` is FNV-1a 64 over the document without `hash`. It detects truncated, damaged and hand-edited files
  (`save_corrupted`); it is an integrity check, not a protection against cheating.
- Values that plain JSON would blur keep a tag: `{"$vec": [...]}`, `{"$color": [...]}`, `{"$entity": id}`.

## Versions and migrations

Bump `game.json` `saves.version` whenever the game's saved data changes shape (a var renamed, a component field
moved). Loading an older save runs one migration step per version, in order:

1. a C++ migrator registered for that version (`engine.saves().addMigrator(1, fn)`, for native modules), else
2. the Wander file named by `saves.migrate`: `fn migrate(from: number, data: map) -> map`, else
3. nothing (the save loads as is, with a warning).

`data` holds `entities`, `globals`, `meta`, `scene` and `destroyed`. Maps and lists are values in Wander: edit
copies and assign them back.

```text
-- scripts/save_migrate.wander: v1 called the player's money `gold`, v2 calls it `coins`
fn migrate(from: number, data: map) -> map
  if from == 1 then
    let out = []
    for e in data.entities
      if e.key == "id:player" then
        let ent = e.entity
        let vars = ent.vars
        vars.set("coins", vars.get("gold", 0))
        vars.remove("gold")
        ent.vars = vars
        e.entity = ent
      end
      out.push(e)
    end
    data.entities = out
  end
  return data
end
```

A save written by a newer version of the game is refused with `save_too_new`.

## How a load works

Between two ticks, while playing:

1. If the save was made in another scene (or in the same scene entered with other entity ids), the scene flow
   changes to it at once, without a transition or loading scene, and its entities get the ids they had in the save.
   Entities with a `persistent` component come along and keep running, as on any scene change; persisted ones
   among them are then restored like the others; sub-scenes loaded with `load_additive` go with the scene they were
   loaded into. A scene change or transition under way is dropped. The editor's play snapshot is untouched.
2. Tombstoned entities are destroyed, and so are persisted entities spawned after the save.
3. Each saved entity is restored in place (keeping its current scripts), or recreated with its saved id and children
   when it was spawned.
4. Entity order (behaviors run in scene order), the next entity id (so later spawns get the same ids), game vars and
   the exact Wander state are restored; the physics world is rebuilt from the restored scene.
5. `on loaded` is delivered on the next tick.

The result is deterministic: saving at tick N, loading, and running M ticks gives the same world as running N + M
ticks directly (tests/test_save_games.cpp checks it, with random numbers, timers, waiting handlers, state machines
and spawns, and for a save loaded from another scene). Stopping play still returns to the scene as it was edited.

`persist` and the scene flow's `persistent` answer different questions: `persist` says what a save restores,
`persistent` says what survives a scene change. A player usually has both. Entities carried across scene changes keep
the ids they had, so carry entities from the scene play starts in (or spawn them in a fixed order) and give persisted
ones a `persist.id`: a save then finds them in every play session.

## Recipe: checkpoints and autosave

A game manager owns the save logic; checkpoints are triggers that tell it when to save.

```text
-- GameManager (persist: {id: "game"}), a ui canvas or an `always` process entity
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
on action "quicksave"       -- quicksave / quickload actions defined in input.json
  save_game("quicksave", {title: "Quicksave"})
end
on action "quickload"
  if not load_game("quicksave") then log "nothing to load" end
end
on ui "Continue"
  load_game("autosave")
end
on tick
  every 120 seconds        -- a timed autosave as a safety net
    save_game("autosave", {title: "Autosave"})
  end
end
on saved
  find("SavingIcon").ui.visible = true
  wait 1
  find("SavingIcon").ui.visible = false
end
on loaded
  find("Chapter").text.text = "Chapter " + str(game_var("chapter"))
end
```

```text
-- Checkpoint (a trigger collider with the "checkpoint" tag)
param title = "Checkpoint"
on trigger_enter "player"
  emit "checkpoint" with {name: self.name, title: title} to find("GameManager")
end
```

Mark what the save must restore: `persist: {id: "player"}` on the player, `persist: {}` on doors and pickups placed in
the scene, `persist: {spawned: true}` on the root of spawned enemy prefabs. Then verify:

```text
sim_control {"action": "play"}
sim_control {"action": "step", "ticks": 600}
save_game {"slot": "test"}
sim_control {"action": "step", "ticks": 300}
load_game {"slot": "test"}
save_inspect {"slot": "test"}      -- every entity "same": nothing was lost
```

## Debugging a load

`save_inspect` compares a slot with the running game. Right after `load_game` every persisted entity should read
`same`. Anything else is what the load could not restore:

| Status | Meaning |
|---|---|
| `differs` | Field paths with the saved and current values (e.g. `components.transform.position`) |
| `missing` | In the save, not in the scene: spawned (a load recreates it) or a scene entity that was deleted from the scene file |
| `destroyed_in_save` | Destroyed before the save; a load removes it |
| `not_in_save` | Persisted now but not in the save: spawned after it (a load removes it) or given `persist` later |

Warnings from `load_game` name behaviors that changed since the save (they restart with their vars kept) and
entities that could not be restored.

## Limits

- Only persisted entities are restored. Particles, animation poses, sounds playing and non-persisted entities keep
  their current state.
- Physics bodies restart from their saved transform and `body.velocity`; contact and sleep state is rebuilt.
- A behavior whose script changed since the save starts fresh (its `on start` runs again; vars keep their saved
  values because var initializers never overwrite existing values).
- Events queued for non-persisted entities at save time are not saved.
