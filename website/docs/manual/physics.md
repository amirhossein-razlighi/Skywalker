# Physics and navigation

Skywalker simulates rigid bodies, character controllers, trigger zones and joints with [Jolt Physics](https://github.com/jrouwe/JoltPhysics) (v5.6.0), and finds paths with [Recast/Detour](https://github.com/recastnavigation/recastnavigation) (v1.6.0), including crowd avoidance for many agents. Physics is part of the deterministic fixed tick: the same scene and inputs replay identically. You set it up with a few components, usually through presets, and control it from Wander and from tools.

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/physics-details.webp · The Details panel of a crate with its body and collider components expanded (motion, mass, friction, shape)</div>

## Concepts

All dimensions are meters, angles are degrees and rotations are Euler degrees, like transforms. Every field is readable and writable from Wander (`self.body.mass`, `self.collider.isTrigger`, ...).

| Component | Role |
|---|---|
| `collider` alone | Static geometry: floors, walls, terrain |
| `body` (with or without `collider`) | A rigid body: dynamic, kinematic or static |
| Child entities with `collider` | Extra parts of the parent body's compound shape |
| `character` | A capsule character controller |
| `joint` | Connects this body to another body or to the world |
| `physics_world` | Scene-wide settings (one per scene) |
| `navmesh` | Navigation mesh bake settings and the saved bake |
| `nav_agent` | A path-following agent with local avoidance |

### body

| Field | Default | Meaning |
|---|---|---|
| `motion` | `dynamic` | `dynamic` (forces, gravity, collisions), `kinematic` (follows its transform and pushes dynamic bodies: platforms, doors), `static` (never moves) |
| `mass` | 1 | kg (crate 20, person 70, car 1200) |
| `friction`, `restitution` | 0.5, 0 | Surface friction; bounciness 0..1 |
| `linearDamping`, `angularDamping` | 0.05 | Drag |
| `gravityScale` | 1 | 0 floats, −1 falls up |
| `lockPosition`, `lockRotation` | `none` | World axes that cannot move or rotate: `"z"` for 2D games, `"xz"` keeps a body upright |
| `ccd` | false | Continuous collision detection: fast objects never tunnel through walls |
| `startAwake` | true | false = sleeps until something touches it |
| `velocity`, `angularVelocity` | 0 | Initial values; live while playing (write them to launch or stop a body) |
| `layer` | `default` | Collision layer |

A body needs no `collider`: its shape is fitted to the entity's mesh. Child entities that have a `collider` but no `body` of their own become parts of the body's compound shape: a cart from a box and four wheels, a table from a top and legs.

### collider

`shape` is one of:

| Shape | Fields | Notes |
|---|---|---|
| `auto` (default) | | Fitted to the mesh. Primitives get the exact primitive (a cube is a box; a plane is a 20 cm slab under the surface). Imported models get a triangle mesh when static and a convex hull when dynamic. |
| `box` | `size` (full size) | |
| `sphere` | `radius` | |
| `capsule`, `cylinder` | `radius`, `height` | Along local Y |
| `convex` | | Convex hull of the mesh |
| `mesh` | | Exact triangles; static and kinematic bodies only (dynamic bodies get a hull, with a warning) |
| `heightfield` | `heightmap`, `size`, `resolution` | Terrain: a square 16-bit `.r16`/`.raw` file or `.hdr`, with `size` = [x extent, max height, z extent]; without a heightmap, the mesh sampled on a `resolution`² grid (8–1024) |

`offset` and `rotation` place the shape relative to the entity; dimensions scale with the entity. `friction` and `restitution` of 0 or more override the body's values for this collider (an ice patch, a bouncy pad; −1 uses the body's). `isTrigger` turns the collider into a sensor zone that reports overlaps and never blocks.

Terrains created with `terrain_create` get a matching `heightfield` collider automatically (see [Terrain](world/terrain.md#physics)).

### character

A capsule moved by Jolt's virtual character. It walks up slopes up to `maxSlope` and steps up to `stepHeight`, slides along walls, rides moving platforms, pushes dynamic bodies (up to `pushStrength` N), collides with other characters and is detected by triggers.

| Field | Meaning |
|---|---|
| `height`, `radius` | Capsule size in meters (not scaled by the transform) |
| `offset` | Capsule centre relative to the entity origin: 0 for a capsule mesh, [0, height/2, 0] for models with the origin at the feet (`physics_add` fits it) |
| `maxSlope`, `stepHeight` | Steepest walkable slope (degrees) and highest curb climbed automatically (m) |
| `moveSpeed`, `jumpSpeed`, `gravity`, `airControl` | Movement: `walk()` with a unit direction moves at `moveSpeed` |
| `turnSpeed` | Degrees per second to turn toward the walk direction (yaw only; 0 = off) |
| `mass`, `pushStrength`, `layer` | Interaction with bodies |
| `velocity` | Live while playing; write it for knockback or launch pads |

### joint

| `kind` | Use | Key fields |
|---|---|---|
| `fixed` | Weld two bodies | |
| `hinge` | Doors, wheels, levers | `axis`, `limitMin`/`limitMax` (degrees), motor |
| `ball` | Chains, pendulums | |
| `slider` | Pistons, drawers | `axis`, limits (m), motor |
| `distance` | A rod, or a rope with `limitMin`/`limitMax` | `connectedAnchor` |
| `spring` | A bouncy link | `stiffness` (Hz), `damping` (ratio) |

`target` names the other entity (empty = attached to the world). `anchor` is the pivot in this entity's local space; for distance and spring joints the other end is `connectedAnchor` in the target's space (a world point for world joints). A motor runs when `motorForce` > 0, toward `motorSpeed`. Joints break above `breakForce` N: `enabled` becomes false and the entity receives `on event "joint_broken"`. `collideConnected` lets the two bodies collide. Each entity has one joint; build a chain by jointing each link to the previous one.

### physics_world and collision layers

`physics_world` holds `gravity` (default Earth, [0, −9.81, 0]), `substeps` (1–8; more is stabler for tall stacks and fast objects), `ignorePairs`, `allowSleep` and `enabled`. Put it on any one entity; `physics_settings` creates an entity named `Physics` for it. Without one, the defaults apply.

The layers are `default`, `static`, `player`, `enemy`, `projectile`, `trigger` and `debris`. Every pair collides except the pairs listed in `ignorePairs` (default `"debris-player, debris-enemy"`, so cosmetic debris never trips characters). Static colliders without a body use `static`; triggers without a body use `trigger`. Queries can filter by layer.

### The simulation step

Each fixed 1/60 s tick:

1. Wander runs: scripts call `walk`, `impulse`, `navigate`, set velocities.
2. The physics world syncs with the scene: new or enabled entities get bodies, destroyed or disabled ones lose them, edited components rebuild their body, entities moved by scripts or tools are teleported, kinematic bodies move to their transforms.
3. Navigation steers agents (crowd simulation).
4. Characters move, then the physics step runs.
5. Dynamic bodies and characters write position, rotation and velocity back to their transforms.
6. Contacts become Wander events for the next tick.

The physics world is built when play starts and discarded on stop, when the scene snapshot is restored.

### Determinism

Jolt is built in cross-platform deterministic mode, entities are processed in scene order, constraints have unique priorities and every callback-derived event is sorted. The same scene and inputs replay identically, which `sim_trace`, playtests and `wander_test` rely on.

### Pause and time scale

While the game is paused (`pause_game()`), the physics world holds: no step and no contacts; bodies keep their velocities and continue on resume. Give the entity that holds `physics_world` a `process` component with `mode: "always"` to keep physics running under a pause menu. `time_scale(x)` steps the world by `dt × x`; above 1 the step is split into substeps no longer than a tick, so fast-forward stays stable. Real-time frames show bodies between ticks (render interpolation); call `teleport(e, position)` after moving a body far in one tick so it is not smeared across the screen.

### Queries while editing

While you edit, queries run against an edit-time mirror of the scene that resyncs whenever the scene changes, so `physics_query`, `nav_path` and `nav_debug` work without pressing play.

## How to make things physical

=== "Tool call"

    ```tool
    physics_add {"entity": "Level", "preset": "static_level"}
    physics_add {"entities": ["Crate 1", "Crate 2", "Barrel"], "preset": "prop"}
    physics_settle {"entities": ["Crate 1", "Crate 2", "Barrel"], "seconds": 4}
    physics_debug {"view": "top"}
    sim_control {"action": "step", "ticks": 120}
    ```

=== "Component"

    ```tool
    entity_update {"entity": "Ball", "components": {"body": {"motion": "dynamic", "mass": 0.45, "restitution": 0.7}, "collider": {"shape": "sphere", "radius": 0.11}}}
    ```

=== "CLI"

    ```bash
    skywalker call physics_add '{"entity": "Level", "preset": "static_level"}' --project .
    ```

**Presets** for `physics_add`:

| Preset | Result |
|---|---|
| `prop` | Dynamic body, collider fitted to the mesh, mass estimated from its size |
| `static_level` | Static colliders for every mesh under the entity (triangle-exact for imported models); use on level roots |
| `kinematic_platform` | Moved by its transform or scripts; carries and pushes things |
| `player_character`, `npc_character` | A capsule character controller fitted to the mesh |
| `trigger_zone` | A sensor firing `on trigger_enter` and `on trigger_exit` |
| `debris` | Light, does not collide with characters |
| `projectile` | Fast, with continuous collision detection |
| `remove` | Strips body, collider, character and joint |

`overrides` patches the created components, for example `{"body": {"mass": 80, "restitution": 0.6}, "collider": {"shape": "sphere"}}`.

`physics_settle` drops the listed objects with a real simulation (up to `seconds`, stopping early when everything sleeps) and keeps their resting poses as one undoable edit. Only the listed entities move; it adds no components.

`physics_debug` captures the viewport with every collider drawn as a wireframe (orange awake, blue-grey asleep, green static, cyan kinematic, magenta trigger, yellow character) plus statistics and warnings.

## Wander

### Events

Contact handlers run on the next tick. The optional filter matches the other entity's name or one of its tags.

| Trigger | Fires when |
|---|---|
| `on collide "name or tag"` | Two bodies started touching (begin only) |
| `on trigger_enter "name or tag"` | Something entered this trigger, or this entity entered a trigger |
| `on trigger_exit "name or tag"` | Something left the trigger |

Inside them, `other` is the other entity, `contact_point` and `contact_normal` (pointing toward `self`) describe the contact, and `impact` is the approach speed in m/s.

### Builtins

| Builtin | Meaning |
|---|---|
| `push(e, force)` | Continuous force in N for this tick: thrusters, wind |
| `impulse(e, vec)` | Instant kick in N·s: jumps, explosions |
| `torque(e, vec)` | Torque in N·m |
| `velocity(e)` | Current velocity of a body or character; set it with `e.body.velocity = ...` |
| `raycast(origin, dir, max)` | First collider hit (not `self`, not triggers) or none; sets `hit_point`, `hit_normal`, `hit_distance` |
| `overlap_sphere(center, r, tag)` | Nearest overlapping entity (not self), optionally with a tag, or none |
| `walk(e, dir)`, `jump(e, speed)`, `grounded(e)` | Character control |
| `navigate(e, point or entity)` | Walk a nav agent there; an entity target is followed as it moves |
| `stop_navigation(e)`, `arrived(e)` | Stop; check arrival |
| `path_length(a, b)` | Walking distance on the navmesh, none if unreachable |

### A player character

```wander
behavior PlayerMove
  intent "Move with WASD relative to the world, jump with space when on the ground."
  on tick
    let dir = (0, 0, 0)
    if key("w") then dir = dir + (0, 0, -1) end
    if key("s") then dir = dir + (0, 0, 1) end
    if key("a") then dir = dir + (-1, 0, 0) end
    if key("d") then dir = dir + (1, 0, 0) end
    walk(self, normalize(dir))      -- |dir| = 1 walks at moveSpeed; stop calling it to stop
  end
  on key "space"
    jump(self)                      -- only when grounded; returns true if it jumped
  end
end
```

`walk` must be called every tick while the character moves. For input actions and gamepads, use `axis("move")` instead of keys (see [Input](input.md)).

### Collisions, triggers and forces

```wander
behavior Grenade
  intent "Thrown forward on spawn; after two seconds it explodes, kicking nearby enemies away."
  on start
    impulse(self, forward(self) * 8 + (0, 4, 0))
  end
  on tick
    after 2 seconds
      burst(80)
      let victim = overlap_sphere(self.position, 4, "enemy")
      if exists(victim) then
        impulse(victim, direction(self, victim) * 50)
      end
      destroy self
    end
  end
end

behavior Coin
  intent "Collected when the player touches it."
  on trigger_enter "player"
    emit "coin_collected"
    destroy self
  end
end

behavior Crate
  intent "Play a thud when hit hard."
  on collide
    if impact > 3 then
      play_sound("audio/thud.wav", min(1, impact / 10))
    end
  end
end
```

### Ground checks with a ray

```wander
behavior Hover
  intent "Hover 1.5 m above whatever is below, like a drone."
  param height = 1.5 in 0.2..5 "hover height (m)"
  on tick
    let ground = raycast(self, (0, -1, 0), 10)
    if ground then
      let error = height - hit_distance
      push(self, (0, 9.81 * self.body.mass + error * 40 - velocity(self).y * 8, 0))
    end
  end
end
```

## Navigation

### The navmesh

Walkable surfaces come from static colliders and static meshes (`navmesh.geometry`: `both`, `colliders` or `meshes`). Anything that moves (dynamic and kinematic bodies, characters, nav agents and their children), triggers, invisible meshes and water are ignored; tag an entity `nav_ignore` to leave it out. The bake is tiled, so large worlds build tile by tile.

| `navmesh` field | Meaning |
|---|---|
| `agentRadius` | Walls are eroded by it |
| `agentHeight` | Lower ceilings block |
| `maxClimb`, `maxSlope` | Highest step (m) and steepest walkable slope (degrees) |
| `cellSize`, `cellHeight` | Voxel precision (agentRadius/2 to /3 is typical) |
| `tileSize` | Cells per tile side (16–256) |
| `data` | The baked file, written by `nav_build` |
| `autoBuild` | Rebuild when play starts if the saved bake is missing or stale |

`nav_build` bakes, saves the result next to the scene (`<scene>.navmesh`: the Detour tiles plus a hash of the input geometry) and records the settings and file in the scene. When play starts, the saved navmesh loads if its hash matches the level; if the level changed and `autoBuild` is on, it is rebuilt in memory with a note to run `nav_build` again. Without any `navmesh` component, a navmesh is baked with default settings the first time something needs one.

### Agents

`nav_agent` has `speed`, `acceleration`, `radius`, `height`, `stoppingDistance`, `turnSpeed`, `avoidance` (`none`, `low`, `medium`, `high`), `destination` with `navigating`, and `autoRepath`. Agents avoid each other, are updated in scene order (deterministic), receive `on event "arrived"` and set `navigating` to false on arrival. With a `character` component the agent walks the character controller (slopes, steps, physics); without one it moves its transform along the navmesh, keeping its height above it.

```wander
behavior Guard
  intent "Chase the player when within 15 m; report when caught."
  on tick
    let player = find("Player")
    if exists(player) and distance(self, player) < 15 then
      navigate(self, player)          -- follows the moving player
    end
  end
  on event "arrived"
    log "caught you"
  end
end
```

Set a destination from a tool instead:

```tool
entity_update {"entity": "Guard", "components": {"nav_agent": {"destination": [12, 0, -4], "navigating": true}}}
```

### Check that a level is traversable

```tool
nav_build {"agent_radius": 0.4, "agent_height": 1.8, "max_climb": 0.4, "max_slope": 45}
nav_path {"from": "Spawn", "to": "Exit"}
nav_debug {"from": "Spawn", "to": "Exit"}
```

`nav_path` returns the corner points, the walking length and whether the goal is reachable (a partial path ends at the closest reachable point). `nav_debug` draws a top-down map with the navmesh in teal, agents as yellow dots with their paths, and the test path in orange: the fastest way to spot unreachable rooms and doorways too narrow for agents.

## Recipes

**Natural clutter.** `scatter` books, rocks or crates above a surface, `physics_settle` them, then `physics_debug` to check that nothing floats.

**A playable character.**

```tool
physics_add {"entity": "Level", "preset": "static_level"}
physics_add {"entity": "Hero", "preset": "player_character", "overrides": {"character": {"moveSpeed": 5, "jumpSpeed": 6}}}
behavior_set {"entity": "Hero", "name": "PlayerMove", "intent": "Move with WASD, jump with space.", "source": "on tick\n  let dir = (0, 0, 0)\n  if key(\"w\") then dir = dir + (0, 0, -1) end\n  if key(\"s\") then dir = dir + (0, 0, 1) end\n  walk(self, dir)\nend\non key \"space\"\n  jump(self)\nend"}
sim_input {"hold": ["w"]}
sim_trace {"entities": ["Hero"], "properties": ["transform.position", "character.velocity"], "ticks": 120, "hold": ["w"]}
```

**A door on a hinge.** A dynamic body with `joint {"kind": "hinge", "anchor": [-0.5, 0, 0], "axis": [0, 1, 0], "limitMin": -100, "limitMax": 0}` attached to the world.

**A moving platform.** `physics_add` with `kinematic_platform`, then move it in Wander (`move self by ...`); bodies and characters on top ride along.

**A hanging chain.** A row of links, each with a `ball` joint whose `target` is the previous link; the first link has an empty `target`, so it hangs from the world.

**Pickups and checkpoints.** `physics_add` with `trigger_zone` on an invisible box, then `on trigger_enter "player"` in Wander.

**Will it fit?** `physics_query` with `type` `overlap` checks that a spot is free before spawning; `shapecast` shows where a dropped object lands:

```tool
physics_query {"type": "overlap", "origin": [4, 1, 2], "shape": "box", "half_extents": [0.5, 0.5, 0.5]}
physics_query {"type": "shapecast", "origin": [4, 6, 2], "direction": [0, -1, 0], "shape": "sphere", "radius": 0.4}
```

`raycast` (the world tool) hits render meshes triangle-exactly, which suits placement on visuals; `physics_query` hits colliders, which is what gameplay sees.

## Pitfalls

- **One joint per entity.** Chains joint each link to the previous one. There is no ragdoll preset.
- **Baked navmesh obstacles.** Moving obstacles are avoided by agents (crowd avoidance) but do not carve the navmesh. There are no off-mesh links (jumps, ladders).
- **Collide events are begin-only.** There is no collide-end event; trigger exit is reported.
- **Heightmap formats.** Heightfield colliders read `.r16`/`.raw` (16-bit) and `.hdr`, not PNG. Terrains write their own `.r16`.
- **Non-uniform scale under rotated parents** uses the product of scales.
- **Editing static colliders during play** takes effect within 8 ticks: static geometry is re-validated every 8th tick (teleports apply every tick).
- **`mesh` colliders on dynamic bodies** become convex hulls; use compound children for concave dynamic objects.

!!! agent "For agents"

    Make things physical with presets, check colliders visually, then verify behavior numerically:

    ```tool
    physics_add {"entity": "Level", "preset": "static_level"}                         # static world
    physics_add {"entities": ["Crate 1", "Crate 2"], "preset": "prop"}                 # dynamic props
    physics_debug {"view": "top"}                                                      # colliders match visuals?
    nav_path {"from": "Spawn", "to": "Exit"}                                           # reachable?
    sim_trace {"entities": ["Crate 1"], "properties": ["transform.position"], "ticks": 120}
    ```

    Use `physics_query`, not `raycast`, for gameplay questions: it sees colliders, triggers and layers exactly as the simulation does.

## Reference

- Tools: [`physics_add`](../reference/tools/physics.md#physics_add), [`physics_query`](../reference/tools/physics.md#physics_query), [`physics_settle`](../reference/tools/physics.md#physics_settle), [`physics_debug`](../reference/tools/physics.md#physics_debug), [`physics_settings`](../reference/tools/physics.md#physics_settings), [`nav_build`](../reference/tools/physics.md#nav_build), [`nav_path`](../reference/tools/physics.md#nav_path), [`nav_debug`](../reference/tools/physics.md#nav_debug), [`sim_trace`](../reference/tools/sim.md#sim_trace).
- Components: [`body`](../reference/components/physics.md#body), [`collider`](../reference/components/physics.md#collider), [`character`](../reference/components/physics.md#character), [`joint`](../reference/components/physics.md#joint), [`physics_world`](../reference/components/physics.md#physics_world), [`navmesh`](../reference/components/physics.md#navmesh), [`nav_agent`](../reference/components/physics.md#nav_agent).
- Wander: [physics](../reference/wander.md#physics), [character](../reference/wander.md#character), [navigation](../reference/wander.md#navigation).
- Related pages: [Simulation and time](simulation.md), [Terrain](world/terrain.md), [Input](input.md).
- Design document: [docs/PHYSICS.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/PHYSICS.md).
