# Physics and navigation

Skywalker simulates rigid bodies, characters, triggers and joints with
[Jolt Physics](https://github.com/jrouwe/JoltPhysics) (the engine behind Horizon Forbidden West),
and finds paths with [Recast/Detour](https://github.com/recastnavigation/recastnavigation) (the
navmesh library behind Unity's and Unreal's navigation). Everything is reachable from the editor,
from Wander, and from agent tools.

The component model mirrors Unity and Unreal, so what you know transfers:

| Skywalker | Unity | Unreal |
|---|---|---|
| `collider` alone | Collider (static) | Static mesh collision |
| `body` + `collider` | Rigidbody + Collider | Simulate Physics |
| child entities with `collider` | compound colliders | multiple collision primitives |
| `character` | CharacterController | Character Movement |
| `joint` | Joint components | Physics Constraint |
| `physics_world` | Physics settings | World/Project settings |
| `navmesh` | NavMeshSurface | Nav Mesh Bounds + RecastNavMesh |
| `nav_agent` | NavMeshAgent | AI controller + path following |

## Quick start for agents

```jsonc
// 1. Floors and walls become static geometry (exact triangles for imported models).
{"tool": "physics_add", "args": {"entity": "Level", "preset": "static_level"}}
// 2. Props become dynamic bodies with a fitted collider and an estimated mass.
{"tool": "physics_add", "args": {"entities": ["Crate 1", "Crate 2", "Barrel"], "preset": "prop"}}
// 3. Drop them so they rest naturally (one undoable edit).
{"tool": "physics_settle", "args": {"entities": ["Crate 1", "Crate 2", "Barrel"]}}
// 4. Check the colliders.
{"tool": "physics_debug", "args": {"view": "top"}}
// 5. Play.
{"tool": "sim_control", "args": {"action": "step", "ticks": 120}}
```

## Components

All dimensions are meters, angles degrees, rotations Euler degrees (like transforms). Every field
is readable and writable from Wander (`self.body.mass`, `self.collider.isTrigger`, ...).

### `body`: a rigid body

| Field | Default | Meaning |
|---|---|---|
| `motion` | dynamic | `dynamic` (forces, gravity, collisions), `kinematic` (follows its transform, pushes dynamic bodies: platforms, doors), `static` (never moves) |
| `mass` | 1 | kg |
| `friction`, `restitution` | 0.5, 0 | surface friction, bounciness (0..1) |
| `linearDamping`, `angularDamping` | 0.05 | drag |
| `gravityScale` | 1 | 0 floats, negative falls up |
| `lockPosition`, `lockRotation` | none | axes that cannot move / rotate (`"z"` for 2D games, `"xz"` keeps a body upright) |
| `ccd` | false | continuous collision detection for fast projectiles |
| `startAwake` | true | false = sleeps until something touches it |
| `velocity`, `angularVelocity` | 0 | initial values; live while playing (write them to launch or stop a body) |
| `layer` | default | collision layer (see below) |

A body needs no `collider`: the shape is fitted to the entity's mesh. Child entities that have a
`collider` but no `body` of their own become parts of the body's compound shape (a cart from a box
and four wheels, a table from a top and legs).

### `collider`: a collision shape

`shape` is one of:

- `auto` (default): fitted to the mesh. Primitives get the exact primitive (cube = box,
  sphere, capsule, cylinder, plane = a 20 cm slab under the surface). Imported models get a
  triangle mesh when static and a convex hull when dynamic.
- `box` (`size` = full size), `sphere` (`radius`), `capsule`, `cylinder` (`radius`, `height`).
- `convex`: convex hull of the mesh.
- `mesh`: the exact triangles (static or kinematic bodies only; dynamic bodies get a hull, with a
  warning).
- `heightfield`: terrain. Either a 16-bit square heightmap (`.r16`/`.raw`, little endian) or
  `.hdr` in `heightmap` with `size` = [x extent, max height, z extent], or, with no heightmap, the
  entity's mesh sampled on a `resolution` x `resolution` grid.

`offset` and `rotation` place the shape relative to the entity; dimensions scale with the entity.
`friction` / `restitution` >= 0 override the body's values for this collider (ice patch, bouncy
pad). `isTrigger` turns it into a sensor zone (see Events).

A collider without a body is static geometry.

### `character`: a character controller

A capsule moved by Jolt's `CharacterVirtual`. It walks up slopes up to `maxSlope` and steps up to
`stepHeight`, slides along walls, rides moving platforms, pushes dynamic bodies (`pushStrength`),
collides with other characters and is detected by triggers. Its dimensions are meters, not scaled
by the transform. `offset` is the capsule center relative to the entity origin: 0 for a capsule
mesh, `[0, height/2, 0]` for models whose origin is at the feet (physics_add fits it for you).
`velocity` is live (write it for knockback or launch pads). `turnSpeed` turns the entity toward
the walk direction (yaw only).

Drive it from Wander every tick:

```wander
behavior PlayerMove
  on tick
    let dir = (0, 0, 0)
    if key("w") then dir = dir + (0, 0, -1) end
    if key("s") then dir = dir + (0, 0, 1) end
    if key("a") then dir = dir + (-1, 0, 0) end
    if key("d") then dir = dir + (1, 0, 0) end
    walk(self, dir)               -- |dir| = 1 walks at moveSpeed; stop calling it to stop
  end
  on key "space"
    jump(self)                    -- only when grounded; returns true if it jumped
  end
end
```

### `joint`: connect two bodies

`kind`: `fixed` (weld), `hinge` (doors, wheels, levers: `axis`, limits in degrees, motor),
`ball` (chains, pendulums), `slider` (pistons, drawers: `axis`, limits in m, motor), `distance`
(a rod, or a rope with `limitMin`/`limitMax`), `spring` (`stiffness` Hz, `damping`). `target` names
the other entity (`""` = attached to the world). `anchor` is the pivot in this entity's local
space; for distance/spring the other end is `connectedAnchor` in the target's space (a world point
for world joints). A motor runs when `motorForce` > 0 at `motorSpeed`. Joints break above
`breakForce` N: `enabled` becomes false and the entity receives `on event "joint_broken"`.
`collideConnected` lets the two bodies collide. One joint per entity; chain links each joint the
previous link.

### `physics_world`: scene settings

`gravity`, `substeps` (more = stabler tall stacks and fast objects), `ignorePairs`,
`allowSleep`, `enabled`. Put it on any one entity; the `physics_settings` tool creates an entity
named "Physics" for it. Without one the defaults apply (Earth gravity).

### Collision layers

`default`, `static`, `player`, `enemy`, `projectile`, `trigger`, `debris`. Every pair collides
except the pairs listed in `physics_world.ignorePairs` (default `"debris-player, debris-enemy"`:
cosmetic debris never trips characters). Static colliders without a body use `static`; triggers
without a body use `trigger`. Queries can filter by layers.

## Simulation

Each fixed 1/60 s tick (`Engine::step`):

1. Wander runs (scripts call `walk`, `impulse`, `navigate`, set velocities...).
2. The physics world syncs with the scene: new or enabled entities get bodies, destroyed or
   disabled ones lose them, edited components rebuild their body, entities moved by scripts or
   agents are teleported, kinematic bodies move to their transforms.
3. Navigation steers agents (DetourCrowd).
4. Characters move, then the physics step runs.
5. Dynamic bodies and characters write position, rotation and velocity back to their transforms.
6. Contacts become Wander events for the next tick.

The world is built when play starts and discarded on stop (the scene snapshot is restored). The
simulation is deterministic: Jolt is built with `CROSS_PLATFORM_DETERMINISTIC`, entities are
processed in scene order, constraints have unique priorities and all callback-derived events are
sorted. The same scene and inputs replay identically, which `sim_trace` and tests rely on.

While editing, queries run against an edit-time mirror of the scene that resyncs whenever the scene
changes, so `physics_query`, `nav_path` and Wander-free tools work without pressing play.

## Wander

Events (handlers run on the next tick; the optional filter matches the other entity's name or one
of its tags):

```text
on collide "Ground" ... end          -- started touching (begin only)
on trigger_enter "player" ... end    -- something entered this trigger (or this entered a trigger)
on trigger_exit ... end
```

Inside them: `other` (the other entity), `contact_point`, `contact_normal` (pointing toward
`self`), `impact` (approach speed in m/s: how hard it hit).

Builtins:

| Builtin | Meaning |
|---|---|
| `push(e, force)` | continuous force in N for this tick (thrusters, wind) |
| `impulse(e, vec)` | instant velocity change in N s (jumps, explosions, kicks) |
| `torque(e, vec)` | N m |
| `velocity(e)` | current velocity of a body or character; set it with `e.body.velocity = ...` |
| `raycast(origin, dir, max?)` | first entity hit (not `self`, not triggers) or none; then `hit_point`, `hit_normal`, `hit_distance` |
| `overlap_sphere(center, r, tag?)` | nearest overlapping entity (not self), optionally with a tag, or none |
| `walk(e, dir)`, `jump(e, speed?)`, `grounded(e)` | character control |
| `navigate(e, point or entity)` | walk a nav agent there (an entity target is followed as it moves) |
| `stop_navigation(e)`, `arrived(e)` | |
| `path_length(a, b)` | walking distance on the navmesh, none if unreachable |

Examples:

```wander
behavior Grenade
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
  on trigger_enter "player"
    emit "coin_collected"
    destroy self
  end
end

behavior Guard
  on tick
    let player = find("Player")
    if exists(player) and distance(self, player) < 15 then
      navigate(self, player)          -- chases the moving player
    end
  end
  on event "arrived"
    log "caught you"
  end
end
```

## Navigation

### The navmesh

Walkable surfaces come from static colliders and static meshes (`navmesh.geometry`: both,
colliders, meshes). Anything that moves (dynamic/kinematic bodies, characters, nav agents and
their children), triggers, invisible meshes and water are ignored; tag an entity `nav_ignore` to
leave it out. The bake is tiled (large worlds build tile by tile) and controlled by the `navmesh`
component: `agentRadius` (walls are eroded by it), `agentHeight`, `maxClimb`, `maxSlope`,
`cellSize`/`cellHeight` (precision), `tileSize`.

`nav_build` bakes, saves the result next to the scene (`<scene>.navmesh`, a compact binary of the
Detour tiles plus a hash of the input geometry) and records the settings and file in the scene.
When play starts, the saved navmesh is loaded if its hash matches the level; if the level changed
and `autoBuild` is on (default) it is rebuilt in memory with a note to run `nav_build` again.
Without any `navmesh` component, a navmesh is baked with default settings the first time
something needs one.

### Agents

`nav_agent`: `speed`, `acceleration`, `radius`, `height`, `stoppingDistance`, `turnSpeed`,
`avoidance` (none/low/medium/high), `destination` + `navigating`, `autoRepath`. Set a destination
from Wander (`navigate`), a tool (`entity_update` with `{"nav_agent": {"destination": [...],
"navigating": true}}`) or the editor. Agents avoid each other (DetourCrowd), are updated in scene
order (deterministic), receive `on event "arrived"` and set `navigating` to false on arrival.
With a `character` component the agent walks the character controller (slopes, steps, physics);
without one it moves its transform along the navmesh, keeping its height above it.

### Tools

- `nav_build`: bake/save (settings as arguments).
- `nav_path`: path between points or entities: corner points, length, reachable/partial.
- `nav_debug`: top-down map with the navmesh, agents and their paths, plus an optional test path.

## Tools

| Tool | Use |
|---|---|
| `physics_add` | Make entities physical with presets: `prop`, `static_level`, `kinematic_platform`, `player_character`, `npc_character`, `trigger_zone`, `debris`, `projectile`, `remove`; `overrides` patches components |
| `physics_query` | `raycast`, `raycast_all`, `shapecast` (sphere/box/capsule sweep), `overlap`; filters: exclude, triggers, layers. Works while editing |
| `physics_settle` | Simulate chosen objects for up to N seconds while editing and keep their resting poses as one undoable edit (natural prop scattering) |
| `physics_debug` | Viewport capture with collider wireframes (orange awake, blue-gray asleep, green static, cyan kinematic, magenta trigger, yellow character) and stats/warnings |
| `physics_settings` | Gravity, substeps, layer matrix, sleeping, on/off |
| `nav_build`, `nav_path`, `nav_debug` | See Navigation |

`raycast` (the older tool) and `Engine::raycast` still hit render meshes triangle-exactly, which
suits placement on visuals; `physics_query` hits colliders, which is what gameplay sees.

## Agent recipes

**Natural clutter.** `scatter` books/rocks/crates above a surface, `physics_settle` them, then
`physics_debug` to check nothing is floating. Settle only moves the listed entities and adds no
components; undo reverts it in one step.

**A playable character.** `physics_add {"entity": "Hero", "preset": "player_character"}`, give it
the PlayerMove behavior above, `physics_add` the level as `static_level`, then `sim_input` with
`hold: ["w"]` and `sim_control step` to test that it walks, climbs and stops at walls.

**Doors and moving platforms.** A door: `body` + `joint {"kind": "hinge", "anchor": [-0.5, 0,
0], "limitMin": -100, "limitMax": 0}`. A platform: `physics_add` preset `kinematic_platform`, then
move it in Wander (`move self by ...`); bodies and characters on top ride along.

**Pickups and checkpoints.** `physics_add` preset `trigger_zone` on an invisible box, then `on
trigger_enter "player"` in Wander.

**Is the level traversable?** `nav_build`, then `nav_debug` with `from`/`to` to see the route, or
`nav_path` between the spawn and every objective (`reachable: false` flags blocked rooms).

**Physics puzzles.** Use `physics_query` overlap to verify a spot is free before spawning, shapecast
to see where a dropped object lands, and `sim_trace` to record body positions over time.

## Limitations (v0.1)

- One `joint` per entity (chain links joint the previous link); no ragdoll preset yet.
- Navmesh obstacles are baked: moving obstacles are avoided by agents (crowd avoidance) but do not
  carve the navmesh. No off-mesh links (jumps, ladders) yet.
- Collide events are begin-only (no "collide end"); trigger exit is reported.
- Heightmap files: `.r16`/`.raw` (16-bit) and `.hdr`; PNG heightmaps need an image decoder in core.
- Non-uniform scale under rotated parents uses the product of scales (Unity's "lossy scale").
- Large static levels: static geometry is re-validated every 8th tick (teleports every tick), so
  editing a static collider during play takes effect within 8 ticks.

## Implementation

| Path | What |
|---|---|
| `engine/include/skywalker/ecs/PhysicsComponents.h` | the components |
| `engine/include/skywalker/physics/PhysicsWorld.h`, `src/physics/PhysicsWorld.cpp` | Jolt world mirroring a scene: sync, step, write-back, contacts, characters, joints, queries |
| `src/physics/Shapes.cpp` | collider -> Jolt shape (compounds, hulls, meshes, height fields), content-keyed shape cache |
| `src/physics/JoltCommon.cpp` | Jolt setup, layers, conversions |
| `engine/include/skywalker/physics/PhysicsSystem.h` | the engine subsystem: play/edit worlds, Wander hooks |
| `engine/include/skywalker/nav/*`, `src/nav/*` | navmesh bake/save/load/query and the crowd |
| `engine/include/skywalker/wander/PhysicsHooks.h` | the interface behind Wander's physics builtins |
| `src/agent/PhysicsTools.cpp`, `src/agent/NavTools.cpp` | tools |
| `src/physics/DebugDraw.cpp` | overlay drawing for physics_debug / nav_debug |
| `cmake/PhysicsDeps.cmake` | Jolt v5.6.0 and recastnavigation v1.6.0 via FetchContent |
| `tests/test_physics.cpp`, `tests/test_nav.cpp` | tests |
