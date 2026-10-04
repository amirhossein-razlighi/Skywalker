# Linked prefabs and entity links

Two pieces of the scene data model make large projects hold together:

- **Entity links:** component fields that point at another entity (a joint's target, a 2D camera's `follow`, an
  animator's `lookAt`...) store the entity's **id** plus its name. Renames never break them, and duplicating or
  pasting a group keeps the group wired to itself.
- **Linked prefabs:** a placed prefab stays linked to its `.prefab.json`. The scene stores only what you changed
  on each instance (its *overrides*). Editing the prefab updates every instance in every scene.

Both are visible and fixable through tools, so agents can check a scene's health before and after a change.

## Entity links

### Fields

| Component | Field | Points at |
|---|---|---|
| `joint` | `target` | the other body (empty = the world) |
| `camera2d` | `follow` | the entity the camera tracks |
| `animator` | `lookAt` | where the head and spine turn |
| `attach` (bone attachment) | `character` | the entity with the animator (empty = nearest ancestor) |
| `ik` | `character` | the entity with the animator (empty = nearest ancestor) |
| `groom` | `target` | the mesh the hair grows on (empty = itself) |
| `groom` | `colliders` | a list of entities the hair collides with |
| `particles` | `subEmitter` | the gpu emitter spawned where particles die or hit |
| `particles` | `colliders` | a list of entities particles bounce off |

Reflection describes them as `FieldType::Entity` (one link) and `FieldType::EntityList` (a list). Their JSON schema
carries `"x-sky-entity": true` / `"x-sky-entity-list": true`; the editor shows an entity picker for them.

### Writing and reading

A link accepts any of these:

```json
"target": "Door"            // a name, looked up near the entity, then bound to the entity's id
"target": "#12"             // an id
"target": 12                // an id
"target": {"id": 12, "name": "Door"}
"target": null              // no entity ("" also clears it)
"colliders": "Shoulders, Hands"   // lists: an array of links, or comma-separated names
```

It is always written back as `{"id": 12, "name": "Door"}` (or `null`), and the name is always the target's
*current* name, so scene files stay readable after renames.

Name resolution:
- Names are looked up **near** the entity: inside its own prefab instance first, then its own subtree, then its
  parent's subtree, then the whole scene.
- A name is bound to an id when that lookup is unambiguous: a nearby match, or a name that is unique in the
  scene. An ambiguous name stays a name-only link and keeps resolving on use. `entity_refs` lists such
  links as `by_name`.
- A link whose id no longer exists falls back to its name. That repairs JSON that was copied between scenes.

### Copy, duplicate and paste

- `entity_duplicate {"entities": ["Arm", "Hand"]}` copies the entities as one group. A joint from Arm to Hand in
  the copy points at the copied Hand.
- Duplicating Arm alone keeps its link to the original Hand.
- `entity_copy` / `entity_paste` follow the same rules, in the same scene or in another scene loaded later.

### Unique names: `%Name`

Mark an entity unique (`entity_update {"entity": "Muzzle", "unique": true}`, or the **%** toggle in the Details
panel). Wander's `find("%Muzzle")` then finds it within the caller's owner:
- the caller's prefab instance, so every spawned enemy finds its own muzzle;
- or the scene, for entities that are not part of an instance.

A script on an instance root sees the unique names inside its own instance first. Names must be unique within
their owner. `entity_refs` reports duplicates.

```
on start
  let muzzle = find("%Muzzle")      -- this instance's muzzle, not another copy's
  self.camera2d.follow = find("Hero")  -- assign an entity (or a name) to a link field
end
```

In Wander a link field reads as the linked entity, or `none` when it is unset or dangling. A list field reads as
a list of entities.

## Linked prefabs

### Instances

`prefab_instantiate`, `scatter` with a prefab, the asset browser and Wander's `spawn("prefab:...")` all create
**linked instances**. These are ordinary entities (rendering, physics and Wander see nothing special), and each
record remembers:
- the instance root;
- the **pid** of the prefab node it came from. A pid is a stable prefab-local id, stored in the file as `"pid"`.
  Version 1 files without pids get depth-first pids 1..n.

`prefab_create` links the source entity to the new prefab (pass `link: false` to opt out).

### Overrides

An override is any difference between an instance and its prefab. Overrides are computed field by field, so
every way of editing produces them:
- tools;
- the editor;
- the gizmo;
- Wander in edit mode.

| Property | Meaning |
|---|---|
| `light.intensity` | one component field |
| `light` | a component added to or removed from the instance (the value is the whole component, or `null`) |
| `name`, `enabled`, `tags`, `unique`, `behaviors` | record fields (behaviors compare as a whole) |
| `vars.hp` | one variable |

The instance root's name and transform are its **placement**. Placement is listed separately and never applied
to the prefab. Entities added under an instance are **added** entities. Prefab nodes deleted from an instance
are **removed**. Links to entities inside the same instance compare by node, so every copy's internal wiring
counts as "the same as the prefab".

### Scene files

A scene saves each instance as its root record plus a `prefab` block:

```json
{"id": 540, "name": "rock 1", "parent": 539,
 "components": {"transform": {"position": [3, 0, -2], "rotation": [0, 40, 0], "scale": [1.2, 1.2, 1.2]}},
 "prefab": {"source": "prefabs/rock.prefab.json", "guid": "3f2a...",
            "ids": {"2": 541, "3": 542},
            "overrides": [{"pid": 2, "node": "Moss", "property": "mesh.color", "value": "#4a6b2f"}],
            "removed": [3]}}
```

- `ids` keeps the members' entity ids stable across save and load, so links, Wander handles and traces survive.
- Loading expands the instance from the **current** prefab, then applies its overrides.
- Overrides on nodes the prefab no longer has are dropped, with a load warning.
- The prefab is found by GUID first, so moving the file keeps the link.
- If the prefab file is missing, the instance loads as a placeholder. Its saved block is kept verbatim, so saving
  again loses nothing. `scene_load` returns a warning, and `entity_refs` lists it under `broken_prefabs`.

`Scene::toJson()` stays the complete, expanded form, used for play snapshots, undo and sandboxes.
`Scene::toFileJson()` is the collapsed file form. `loadJson` reads both.

### Propagation

When a prefab file changes, every instance in the open scene is re-expanded from the new version and keeps its
overrides. Changes come from:
- an edit on disk, picked up by the periodic asset scan or `refreshAssets`;
- `prefab_apply`.

Entity ids of existing members stay the same. While the game is playing, the update waits until play stops.
Other scenes pick the change up when they are loaded, because they store only differences.

## Tools

| Tool | Use |
|---|---|
| `entity_refs {entity}` | Incoming and outgoing links of an entity, each `ok`, `by_name` or `dangling`, plus its prefab membership. |
| `entity_refs {}` | Scene health: dangling links (with the `entity_update` call that fixes each), name-only links, duplicate unique names, broken prefab instances and load warnings. |
| `entity_copy` / `entity_paste` | Clipboard documents with links and prefab instances intact. |
| `entity_duplicate {entities}` | Duplicate a group with its internal links remapped. |
| `prefab_overrides {entity}` | One instance: overrides (with the prefab's value), removed nodes, added entities, placement. `stale: true` means the file changed and the next scan will update it. |
| `prefab_overrides {}` | Every instance in the scene with its override count. |
| `prefab_revert {entity, property?, all?}` | Revert one override, all of an entity's overrides, or the whole instance (restores removed nodes). Did-you-mean on property typos. |
| `prefab_apply {entity, property?}` | Push overrides into the prefab file. Without `property` it pushes everything, and added entities become prefab nodes and stay linked. Other instances update and keep their own overrides. |
| `prefab_unpack {entity}` | Unlink an instance and keep its entities as they are. |
| `prefab_relink {prefab, entities?, max_overrides?, dry_run?}` | Link existing copies whose hierarchy matches the prefab node for node. Values never change: differences become overrides. |

## Recipes

**Tweak one lamp, then make it the default**

1. `prefab_instantiate {"prefab": "prefabs/lamp.prefab.json", "position": [4, 0, 2]}`.
2. `entity_update {"entity": <the instance's Lamp>, "components": {"light": {"intensity": 6}}}`.
3. `prefab_overrides {"entity": <instance>}` lists `Lamp light.intensity = 6 (prefab: 3)`.
4. Either `prefab_revert {"entity": <Lamp>, "property": "light.intensity"}`, or
   `prefab_apply {"entity": <Lamp>, "property": "light.intensity"}` so every lamp gets 6.

**Rename safely**

1. `entity_refs {"entity": "Door"}` shows what points at it.
2. `entity_update {"entity": "Door", "name": "Gate"}` reports how many links follow the rename. Name-only links and
   Wander `find("Door")` strings do not follow it.
3. `entity_refs {}` confirms nothing is dangling.

**Adopt prefabs in an old scene**

1. `prefab_relink {"prefab": "prefabs/tree.prefab.json", "dry_run": true}`.
2. Run it again without `dry_run`, then `scene_save`. The file shrinks to roots plus overrides, and loads exactly
   as before.

## Limits (planned next)

- No nested prefabs or variants yet. A prefab made from a tree that contains instances flattens them. The
  `prefab_create` result says when an entity could not be linked for that reason.
- Behaviors and tags compare as whole values: editing one script overrides the whole behavior list.
- Animation sequences (`*.sequence.json`) still name their targets.
