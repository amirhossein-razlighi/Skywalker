---
name: skywalker-physics
description: "Physics and navigation in Skywalker - Jolt rigid bodies, colliders, character controllers, triggers and joints, wheeled vehicles (cars, trucks, karts with suspension, gearbox, assists, drifting, test drives), collision queries, settling props into natural poses, debug views, and Recast navmesh pathfinding with nav agents. Use for falling crates, a walking/jumping player, doors and platforms, pickups and checkpoints, projectiles, drivable cars and racing games, patrolling or chasing AI, and \"is the level traversable?\"."
---

# Physics and navigation

Load skywalker-core first. Engine doc: `skywalker://docs/PHYSICS`. Jolt simulates bodies, characters, triggers and joints; Recast/Detour bakes the navmesh. The simulation is **deterministic**
(fixed 1/60 s ticks, ordered events), so verify with `sim_control step` and `sim_trace`, not by eye. Components mirror Unity/Unreal:
`collider` alone = static geometry, `body` + `collider` = rigid body, `character` = character controller, `joint`, `physics_world` (scene settings), `navmesh`, `nav_agent`.

## Presets: make things physical with `physics_add`

One undoable edit; give `entity` or `entities`. `overrides` patches components.

| Preset | Result |
|---|---|
| `static_level` | Static colliders for every mesh under the entity (triangle-exact for imported models). Use on level roots. |
| `prop` | Dynamic body, collider fitted to the mesh, mass estimated from size |
| `kinematic_platform` | Moved by its transform/scripts; carries and pushes bodies and characters |
| `player_character`, `npc_character` | Capsule character controller fitted to the mesh (drive with `walk`/`jump` in Wander) |
| `trigger_zone` | Sensor: fires `on trigger_enter` / `on trigger_exit` |
| `debris` | Light, does not collide with characters |
| `projectile` | Fast body with continuous collision |
| `remove` | Strips body, collider, character and joint |

```text
physics_add {entity:"Level", preset:"static_level"}
physics_add {entities:["Crate 1","Crate 2","Barrel"], preset:"prop"}
physics_add {entity:"Ball", preset:"prop", overrides:{body:{mass:80, restitution:0.6}, collider:{shape:"sphere"}}}
physics_add {entity:"Hero", preset:"player_character"}
physics_add {entity:"Coin Zone", preset:"trigger_zone"}
physics_settle {entities:["Crate 1","Crate 2","Barrel"], seconds:4}
physics_debug {view:"top"}
sim_control {action:"step", ticks:120}
sim_control {action:"stop"}                                    # restores the pre-play scene; physics exists only while playing
```

## Core workflow

1. **Measure first** (`scene_overview`, `raycast`, `terrain_query`). Interpenetrating colliders at the start explode: place props above the surface, not inside it.
2. **Level**: `physics_add static_level` on the geometry root. Terrain uses a `collider` with `shape:"heightfield"` (a mesh sampled on a `resolution` grid, or a 16-bit heightmap).
3. **Props**: `physics_add prop`, then **`physics_settle`**: simulates only the listed entities for up to N seconds (max 30, stops early when everything sleeps) and keeps the resting poses as one undo step. Ideal after `scatter` for natural clutter.
4. **Check**: `physics_debug` draws collider wireframes (orange awake dynamic, blue-gray asleep, green static, cyan kinematic, magenta trigger, yellow character) with stats and warnings (bad layers, unsupported shapes, missing joint targets).
5. **Behavior**: Wander drives the world (below). Verify with `sim_input`, `sim_control step`, `sim_trace`.
6. **Tune scene-wide**: `physics_settings {gravity:[0,-1.62,0]}` (Moon), `substeps` 1..8 for tall stacks and fast objects, `ignorePairs`, `allowSleep`, `enabled`. Creates an entity named "Physics" with a `physics_world` component on first change; call with no arguments to read.

## Queries (colliders, not render meshes; work while editing)

```text
physics_query {type:"raycast", origin:[0,10,0], direction:[0,-1,0]}                       # first hit; raycast_all = every entity along the ray
physics_query {type:"overlap", origin:[5,1,5], shape:"sphere", radius:1}                  # "free": true when nothing touches: is this spot free to spawn?
physics_query {type:"shapecast", origin:[0,10,5], direction:[0,-1,0], shape:"capsule", radius:0.4, height:1.8, exclude:["Hero"]}   # will it fit / where does it land
```

Triggers are ignored unless `include_triggers:true`; filter with `layers:["enemy"]`. The older `raycast` tool still hits **render meshes** triangle-exactly (good for placement on visuals); `physics_query` hits what gameplay sees.

## Components (all meters/degrees; every field is readable and writable from Wander)

- `body`: `motion` dynamic|kinematic|static, `mass`, `friction`, `restitution`, `linearDamping`, `gravityScale`, `lockPosition`/`lockRotation` (`"z"` for 2D, `"xz"` keeps a body upright), `ccd`, `startAwake`, live `velocity`, `layer`.
  A body needs no collider (the shape is fitted to the mesh); child entities with a `collider` and no `body` become parts of the compound shape (a cart from a box and wheels).
- `collider`: `shape` auto|box|sphere|capsule|cylinder|convex|mesh|heightfield, `size`, `radius`, `height`, `offset`, `isTrigger`, per-collider `friction`/`restitution`. `mesh` is static/kinematic only (dynamic bodies get a hull). Dimensions scale with the entity.
- `character`: capsule on Jolt's CharacterVirtual: `height`, `radius`, `offset` (capsule center vs entity origin; `[0,height/2,0]` for feet-origin models), `maxSlope`, `stepHeight`, `moveSpeed`, `jumpSpeed`, `pushStrength`, `turnSpeed`. Dimensions are meters, not scaled by the transform.
- `joint`: `kind` fixed|hinge|ball|slider|distance|spring, `target` (other entity, `""` = world), `anchor`, `axis`, `limitMin`/`limitMax`, motor (`motorForce`, `motorSpeed`), `breakForce` (then `on event "joint_broken"`). One joint per entity; a chain jointing each link to the previous.
- Layers: `default static player enemy projectile trigger debris`; every pair collides except `physics_world.ignorePairs` (default `"debris-player, debris-enemy"`).

## Wander (verify with `wander_check`)

```wander
behavior PlayerMove
  on tick
    let dir = (0, 0, 0)
    if key("w") then dir = dir + (0, 0, -1) end
    if key("s") then dir = dir + (0, 0, 1) end
    if key("a") then dir = dir + (-1, 0, 0) end
    if key("d") then dir = dir + (1, 0, 0) end
    walk(self, dir)               -- call every tick while walking; stop calling to stop
  end
  on key "space"
    jump(self)
  end
end

behavior Coin
  on trigger_enter "player"       -- optional filter: name or tag of the other entity
    emit "coin_collected"
    destroy self
  end
end
```

Events: `on collide ("Ground")` (begin only), `on trigger_enter`, `on trigger_exit`; inside them `other`, `contact_point`, `contact_normal`, `impact` (m/s).
Builtins: `push(e, force)`, `impulse(e, vec)`, `torque`, `velocity(e)`, `raycast(origin, dir, max?)` then `hit_point`/`hit_normal`/`hit_distance`, `overlap_sphere(center, r, tag?)`, `walk`, `jump`, `grounded`, `navigate`, `stop_navigation`, `arrived`, `path_length(a, b)`.
Tag the player `player` and goals/hazards so `playtest_run` can measure the game (skywalker-studio).

## Recipes

- **Playable character**: `physics_add player_character`, attach `PlayerMove`, `physics_add static_level` on the level, then `sim_input {hold:["w"]}` + `sim_trace {entities:["Hero"], properties:["transform.position"], ticks:120, every:20}`: it must walk, climb steps (`stepHeight`) and stop at walls.
- **Door**: `body` + `joint {kind:"hinge", anchor:[-0.5,0,0], limitMin:-100, limitMax:0}`. **Moving platform**: `physics_add kinematic_platform` and `move self by ...` in Wander; things on top ride along.
- **Pickups / checkpoints**: invisible box + `trigger_zone` + `on trigger_enter "player"`.
- **Launch**: `impulse(self, forward(self) * 8 + (0, 4, 0))`; explosions: `overlap_sphere` + `impulse` away from the center.
- **2D side-scroller**: bodies with `lockPosition:"z"` and `lockRotation:"xy"`; colliders from `tilemap_inspect` rects (see skywalker-2d-ui).

## Vehicles (cars, trucks, karts)

A `vehicle` on a dynamic `body` is a Jolt wheeled vehicle: suspension, tire friction curves, engine torque curve, auto/manual gearbox, FWD/RWD/AWD limited-slip
differentials, anti-roll bars, downforce and drag. Wheels are the chassis' **children named `wheel*`** (radius, width and position measured from their meshes);
they spin, steer and follow the suspension while playing. Forward is **-Z**. Build one with `vehicle_create`, measure it with `vehicle_test_drive`, adjust with `vehicle_tune`:

```text
vehicle_create {entity:"Coupe", preset:"sports"}                       # sports | hatchback | truck | kart; handling arcade (assists) | sim
vehicle_test_drive {entity:"Coupe", maneuver:"all"}                    # 0-100 km/h, braking distance, fastest clean slalom, skidpad g
vehicle_tune {entity:"Coupe", set:{maxTorque:600, lateralGrip:1.2}, test:"accel"}   # before/after numbers; typos get a did-you-mean
vehicle_tune {entity:"Coupe", wheels:{rear:{lateralGrip:1.0}}}         # per axle or wheel (front_left, rear_right...)
vehicle_test_drive {entity:"Coupe", maneuver:"skidpad", overrides:{antiRollRear:900}}   # try a change without editing
vehicle_info {entity:"Coupe", capture:true}                            # telemetry + the "vehicles" debug view (suspension, contacts, tire forces)
input_map {operation:"add_preset", preset:"drive"}                     # throttle/brake/steer/handbrake/shift actions (vehicle_create adds them)
```

- `vehicle_create` adds the body (chassis mass), a box collider above the wheels, the preset, a chase camera on the scene camera (`chase_camera`: spring arm,
  look-ahead, FOV widening with speed), a looping engine sound pitched by rpm, and the drive actions. Model the wheels as separate child meshes: a single mesh
  with the wheels baked in cannot spin them and its collider would touch the ground.
- Inputs are live fields (`throttle`, `brake`, `steer` -1 left..1 right, `handbrake`). `control:"player"` reads the drive actions every tick; Wander overrides
  them with `vehicle_drive(self, throttle, steer, brake?, handbrake?)`; `control:"script"` leaves them to scripts and tools. Holding brake at a standstill reverses.
- Assists (live, no rebuild): `tractionControl` (trims the throttle when wheelspin makes the car slide), `abs` (peak-friction braking, steering while braking),
  `driftAssist` 0..1 (easier, held slides: rear lightens on power, counter-steer only past ~30 degrees), `steerSpeed`, `speedSensitiveSteering`. Pulling the handbrake
  declutches and locks the rear wheels: the way into a drift (flick, then power and a little counter-steer).
- Telemetry is written back each tick (`speed` km/h, `rpm`, `gear`, `wheelsOnGround`, `skid` 0..1). Per wheel in Wander: `vehicle_wheel(self, "rear_left")` has
  `point`, `skid`, `slip_ratio`, `slip_angle`, `load`, `surface`; `vehicle_state(self).skidding` lists sliding contact points: burst tire smoke and spawn skid marks there.
- Tuning by numbers: grip ~ skidpad g (`lateralGrip` 0.9 hatchback, 1.1 sports); stiffer rear anti-roll (`antiRollRear`) = more oversteer; `centerOfMass` lower = less roll;
  `drag` sets top speed; `differentialRatio` and `gearRatios` trade acceleration for top speed. Surfaces with friction < 0.5 (ice 0.05, mud 0.25) scale tire grip.

```wander
behavior TireSmoke
  on tick
    let w = vehicle_wheel(self, "rear_left")
    if w and w.skid > 0.3 then
      burst(find("Smoke RL"), 2)                -- a particles child at the wheel, worldSpace on
    end
  end
end
```

## Navigation (Recast/Detour)

`nav_build` bakes the navmesh from **static** colliders and static meshes (dynamic bodies, characters, triggers, invisible meshes and water are ignored; tag an entity `nav_ignore` to exclude it), saves it next to the scene
(`navmesh/<scene>.navmesh`) and records settings in a `navmesh` component (entity "Navigation"). Re-run after changing the level layout.

```text
nav_build {agent_radius:0.4, agent_height:1.8, max_climb:0.4, max_slope:45, cell_size:0.2}
nav_path {from:"Player", to:[12,0,-4]}                          # corners, length, reachable / partial
nav_debug {from:"Player", to:[12,0,-4], size:800}               # top-down map: teal walkable, yellow agents, orange test path
entity_update {entity:"Guard", components:{nav_agent:{speed:3.5, stoppingDistance:0.5, avoidance:"medium"}}}   # fields: speed acceleration radius height stoppingDistance turnSpeed autoRepath avoidance
```

- `nav_path` `reachable:false` or `partial:true` flags blocked rooms; run it from the spawn to every objective. `nav_debug` is the fastest way to spot doorways narrower than the agent, gaps and islands. Both bake the navmesh first if needed.
- Agents walk the navmesh with crowd avoidance in scene order (deterministic); with a `character` they use the controller (slopes, steps), without one they move the Transform at a fixed height. In Wander: `navigate(self, target)` (point or entity; follows a moving entity),
  `on event "arrived"`, `arrived(e)`, `path_length(a, b)` (none if unreachable).
- A guard that chases: `if distance(self, player) < 15 then navigate(self, player) end` in `on tick`.
- `cell_size` smaller = more precise and slower; `agent_radius` erodes walls (doorways need width above 2 x radius).

## Verification loop

1. `physics_debug` (top view and a perspective `focus`): colliders match the visuals, triggers sit where expected, props fall asleep (blue-gray) instead of jittering.
2. `sim_control step` + `sim_trace` on positions/velocities; repeat the same run twice to confirm determinism (identical numbers).
3. Capture mid-run, not only at the start. If a body tunnels, add `ccd` or raise `substeps`.
4. Navigation: `nav_debug` image plus `nav_path` numbers for every important route.

## Pitfalls

- Starting inside geometry or interpenetrating props: use `physics_settle` or place above the surface (`raycast`, `place_on_surface`).
- Editing a static collider during play takes up to 8 ticks to take effect. Collide events are begin-only; trigger exit is reported.
- Queries while editing use an edit-time mirror that resyncs when the scene changes; physics itself exists only while playing (play changes are discarded on stop).
- Non-uniform scale under rotated parents uses the product of scales. One `joint` per entity. No ragdoll preset, no off-mesh links (jumps, ladders), navmesh does not carve for moving obstacles.
- Characters are not scaled by the transform: size `radius`/`height` explicitly (or let `physics_add` fit them).
- `mesh` colliders on dynamic bodies silently become hulls (with a warning in `physics_debug`).
