# Physics and navigation

Skywalker simulates rigid bodies, characters, triggers and joints with
[Jolt Physics](https://github.com/jrouwe/JoltPhysics), and finds paths with
[Recast/Detour](https://github.com/recastnavigation/recastnavigation). Everything is reachable from
the editor, from Wander, and from agent tools.

The physics components:

| Component | What it is for |
|---|---|
| `collider` alone | Static geometry: floors, walls and level meshes that never move |
| `body` + `collider` | A simulated rigid body (dynamic or kinematic) with its collision shape |
| child entities with `collider` | A compound shape: several colliders under one body |
| `character` | A capsule character controller that walks, climbs steps and slides along walls |
| `joint` | A constraint between two bodies (or a body and the world): fixed, hinge, ball, slider, distance or spring |
| `vehicle` (+ `body`) | A wheeled vehicle: suspension, engine, gearbox, differentials and tyres, driven by input or Wander |
| `physics_world` | Scene-wide settings: gravity, substeps, layer pairs that never collide, sleeping |
| `navmesh` | Navigation-mesh bake settings and the saved bake (one per scene) |
| `nav_agent` | An entity that finds paths on the navmesh and follows them with crowd avoidance |

2D games on the XY plane have their own set, simulated with [Box2D](https://github.com/erincatto/box2d):
`body2d`, `collider2d` (including merged tilemap collision), `joint2d`, `character2d` and
`physics2d_world`. See [2D physics](#2d-physics).

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

**Game pause and time scale.** While the game is paused (`pause_game()`), the physics world
holds: no step, no contacts, bodies keep their velocities and continue on resume (the contacts of
the step before the pause reach their handlers on resume). Give the entity with `physics_world` a
`process` component with `mode: "always"` to keep physics running under a pause menu. `time_scale(x)`
steps the world by `dt * x`; above 1 the step is split into substeps no longer than a tick, so fast
forward stays stable. Real-time frames show bodies between ticks (render interpolation); call
`teleport(e, position)` after moving a body far in one tick.

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
`navigate(self, entity)` tracks the entity while walking (with `autoRepath`) and stops once it
gets there; call it again (every tick or every second) to keep chasing.
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
| `vehicle_create`, `vehicle_tune`, `vehicle_info`, `vehicle_test_drive` | See Vehicles |
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

## Vehicles

A `vehicle` component on an entity with a dynamic `body` makes a drivable wheeled vehicle, simulated by Jolt's vehicle
constraint with its wheeled controller: raycast-free cylinder wheel contacts, spring/damper suspension, tire friction
curves (longitudinal by slip ratio, lateral by slip angle), an engine with a torque curve and inertia, an automatic or
manual gearbox with clutch and shift times, FWD/RWD/AWD limited-slip differentials, anti-roll bars, and aerodynamic
downforce and drag. On top of that the engine adds driving assists, player controls, telemetry, wheel visuals, engine
audio, a chase camera, a debug view and an autopilot that measures handling.

### Setting one up

The chassis entity holds the car model; its wheels are **child entities named `wheel*`** (`wheel_fl`, `wheel_fr`,
`wheel_rl`, `wheel_rr`), modeled as separate meshes. When `vehicle.wheels` is empty they are fitted when play starts:
position, radius and width come from their meshes; wheels are grouped into axles by position (forward is **-Z**), the
front axle steers, and `drive` picks the driven axles. The suspension's rest length is computed from the weight on each
wheel so the car rests at its modeled ride height. `vehicle_create` does all of it in one undoable edit:

```jsonc
{"tool": "vehicle_create", "args": {"entity": "Coupe", "preset": "sports", "handling": "arcade"}}
```

It adds the body (chassis mass, no linear damping), a **box collider fitted to the chassis above the wheel bottoms**
(the wheels themselves never collide; the suspension holds the car up), the preset, a `chase_camera` on the scene
camera, a looping `audio/engine_loop.wav` (synthesized) on the car, and the drive input actions. Its result lists the
fitted wheels and any warnings. A model with the wheels baked into the body mesh cannot spin them: separate them first
(`dcc_*` tools or a modeling app).

### Component fields

| Group | Fields |
|---|---|
| Chassis | `mass` (kg, replaces `body.mass`), `centerOfMass` (offset from the shape's center; lower = less roll), `maxTilt` (degrees before the chassis is held upright; 180 = can flip) |
| Wheels | `wheels` (per-wheel overrides, see below), `wheelRadius`/`wheelWidth` (0 = measured), `suspensionMinLength`/`suspensionMaxLength`, `suspensionFrequency` (Hz), `suspensionDamping`, `steering` (front/rear/all/none), `maxSteerAngle`, `brakeTorque`, `handbrakeTorque` |
| Tires | `longitudinalGrip`, `lateralGrip` (peak friction: roughly the skidpad g), `longitudinalCurve`, `lateralCurve` (`[[slip, 0..1], ...]`, empty = default) |
| Drivetrain | `drive` (fwd/rwd/awd), `frontTorqueSplit` (awd), `limitedSlip` (1.05 locked .. >= 10 open), `differentialRatio` |
| Engine | `maxTorque`, `minRpm`, `maxRpm`, `engineInertia`, `engineDamping`, `torqueCurve` (`[[rpm fraction, torque fraction], ...]`) |
| Gearbox | `transmission` (auto/manual), `gearRatios`, `reverseRatio`, `shiftUpRpm`, `shiftDownRpm`, `shiftTime`, `clutchStrength` |
| Chassis dynamics | `antiRollFront`, `antiRollRear` (0 none, 300 soft, 700 sporty, 2000 very stiff), `downforce`, `drag` (N per (m/s)^2) |
| Control | `control` (player = drive actions, script = Wander/tools, none), `steerSpeed`, `speedSensitiveSteering`, `tractionControl`, `abs`, `driftAssist` (0..1), `autoReverse`, `engineAudio` |
| Inputs (live) | `throttle`, `brake` (0..1), `steer` (-1 left .. 1 right), `handbrake` |
| Telemetry (live) | `gear` (manual: write to shift), `speed` (km/h along the heading), `rpm`, `wheelsOnGround`, `skid` (0..1) |

A `wheels` entry is `{entity, position, radius, width, steer, drive, handbrake, maxSteerAngle, brakeTorque,
handbrakeTorque, suspensionMinLength, suspensionMaxLength, suspensionFrequency, suspensionDamping, longitudinalGrip,
lateralGrip}`; every field is optional and overrides the vehicle default for that wheel (`position` is in the chassis'
local space). `vehicle_tune {wheels: {rear: {...}}}` writes the list from the current fit the first time. Inputs,
assists, aero and the control mode are read live every tick; the other fields rebuild the vehicle (keeping its speed and
engine revs) when they change, also while playing.

Surfaces: a tire gets its full grip on surfaces with friction >= 0.5 (the default; asphalt, concrete) and
proportionally less below (ice 0.05, mud 0.25); per-collider `friction` overrides work, so an ice patch is a collider
with `friction: 0.05`.

### Driving

- **Player**: with `control: "player"` the vehicle reads the input actions `throttle` (W/Up/right trigger), `brake`
  (S/Down/left trigger), `steer` (A-D/arrows/left stick), `handbrake` (Space/gamepad A) and, for manual gearboxes,
  `shift_up`/`shift_down` (E/Q/shoulders) every tick. Projects without those actions get the same default bindings;
  `input_map {operation: "add_preset", preset: "drive"}` writes them to `input.json` for rebinding.
- **Scripts**: `vehicle_drive(self, throttle, steer, brake?, handbrake?)` sets the inputs for this tick (it overrides
  the player); `vehicle_shift(self, gear)` for manual gearboxes. With `control: "script"` only scripts and tools drive.
- Holding brake at a standstill reverses (`autoReverse`); pressing throttle while rolling backward brakes first.
- Pulling the handbrake declutches and locks the rear wheels (a locked tire slides and barely steers): flick into a
  corner, then throttle and a little counter-steer to hold a drift.

Assists (all live): `steerSpeed` rate-limits the steering like a rack, `speedSensitiveSteering` reduces lock at speed,
`tractionControl` trims the throttle when wheelspin steps the car sideways (it leaves straight-line launches alone),
`abs` brakes each tire at its peak friction and keeps its steering, `driftAssist` makes slides easier to start and hold
(the rear lightens on power, speed is kept through the slide, and counter-steer and yaw damping only step in past ~30
degrees of drift so it never turns into a spin).

### Seeing and hooking in

- `vehicle_info` returns live telemetry: speed, rpm, gear, inputs as asked and as applied after the assists, assist
  activity, drift angle, lateral/longitudinal g, and per wheel: suspension length and compression, contact point and
  surface, load, drive and cornering forces, slip ratio (+ spin, - lock) and slip angle, skid. While editing it shows the
  fitted setup at rest. `capture: true` adds a picture with the debug view.
- Debug view **`vehicles`** (`viewport_capture {debug_view: "vehicles"}`): suspension travel (gray) and current length
  (white to orange), wheels (green on the ground, red sliding, gray airborne), contact points, and the tire forces at
  them (blue load, orange drive/brake, red cornering; 0.6 m = a wheel's static load), velocity (cyan) and center of mass
  (yellow). It is a CPU overlay on captures; the live viewport shows the final image.
- Wander: `vehicle_speed(e)`, `vehicle_state(e)` (speed, rpm, gear, wheels_on_ground, skid, drift_angle, lateral_g,
  longitudinal_g, load, `skidding` = contact points of sliding tires) and `vehicle_wheel(e, i or name)` (contact, point,
  normal, surface, load, slip_ratio, slip_angle, skid, compression, steer, rpm, driven). Tire smoke: a `particles` child
  per rear wheel (`worldSpace`, not emitting) and `burst()` it while that wheel's `skid` > 0.3; skid marks: `spawn` dark
  planes at `point` (the Vehicle Yard demo does both).
- Engine audio: with `engineAudio` the entity's `audio` component follows the engine (pitch from rpm, volume from load).
- `chase_camera` (on a camera entity): `target`, `distance`, `height`, `targetHeight`, `lookAhead` (seconds of
  velocity), `stiffness`, `turnStiffness` (the arm swings behind the direction of travel, so drifts stay framed),
  `fovMin`/`fovMax`/`fovSpeed` (wider at speed), `collide` (pulls in when a wall blocks the view).
- `perf_stats.vehicles`: vehicles, wheels and the per-tick CPU cost of the vehicle code (assists, aero, write-back).

### Tuning by numbers

`vehicle_test_drive` drives a copy of the vehicle in a private world (the scene is untouched; it works while editing)
on a flat proving ground with full grip, or `track: "scene"` in the level, and returns metrics:

| Maneuver | Measures |
|---|---|
| `accel` | 0-60 and 0-100 km/h, quarter mile time and speed, upshifts, seconds of wheelspin |
| `braking` | distance and time from `speed` (100 km/h), mean and peak deceleration g, ABS activity, locked wheels, stability |
| `slalom` | 8 cones every `cone_spacing` m; without `speed` it finds the fastest clean run (`maxCleanKmh`) |
| `skidpad` | a `radius` m circle at a rising speed: lateral g, and whether the limit is understeer, oversteer or power |
| `top_speed` | top speed within `duration` s |
| `custom` | `inputs` keyframes `[{t, throttle, brake, steer, handbrake}]`, with `trace: true` for a sampled trace |
| `all` | accel, braking, slalom and skidpad |

`overrides` tries vehicle fields without editing; `vehicle_tune {set: {...}, test: "skidpad"}` edits and returns the
before/after summary. Every run reports `maxLateralG`, `maxDriftAngle` and `maxRollDeg`.

The presets on representative test chassis (proving ground, arcade handling):

| Preset | 0-100 km/h | 100-0 km/h | Fastest clean slalom (18 m) | Skidpad (40 m) | Top speed |
|---|---|---|---|---|---|
| `sports` (1350 kg, 520 N m, RWD, 6 gears) | 4.8 s | 31.6 m | 60 km/h | 1.13 g | 269 km/h |
| `hatchback` (1150 kg, 260 N m, FWD, 5 gears) | 9.1 s | 45.0 m | 55 km/h | 0.94 g | 202 km/h |
| `truck` (2600 kg, 700 N m, AWD, 6 gears) | 8.1 s | 41.6 m | 50 km/h | 0.85 g | 203 km/h |
| `kart` (170 kg, 42 N m, one gear) | 0-60 in 3.2 s | 24.6 m (from 92) | 65 km/h | 0.98 g | 92 km/h |

Typical adjustments: more `lateralGrip` = more cornering g; a stiffer rear anti-roll bar (or less rear grip) = more
oversteer; a lower `centerOfMass` = less roll; `drag` sets the top speed; `differentialRatio`/`gearRatios` trade
acceleration for top speed; `suspensionFrequency` 1.3 soft .. 2.5 race.

### Recipe: a drift car in five calls

```jsonc
{"tool": "vehicle_create", "args": {"entity": "Coupe", "preset": "sports"}}
{"tool": "vehicle_tune", "args": {"entity": "Coupe", "set": {"driftAssist": 0.6, "antiRollRear": 700}, "test": "skidpad"}}
{"tool": "vehicle_test_drive", "args": {"entity": "Coupe", "maneuver": "custom", "trace": true, "inputs": [
  {"t": 0, "throttle": 1}, {"t": 3.4, "throttle": 0.3, "steer": -1, "handbrake": 1},
  {"t": 3.9, "throttle": 1, "steer": -0.3, "handbrake": 0}, {"t": 4.3, "throttle": 1, "steer": 0.3}]}}
{"tool": "sim_input", "args": {"hold": ["w", "a"]}}
{"tool": "vehicle_info", "args": {"entity": "Coupe", "capture": true}}
```

The custom run's `maxDriftAngle` (and the trace's `driftAngle`) shows whether the slide is held (20-40 degrees) or
spins (above 70). `examples/demo_vehicle_yard` is a proving ground with a slalom, a skidpad, ramps and a coupe whose
`ShowcaseDrift` behavior drives this sequence when its `demo` var is true.

Determinism: vehicles step inside the fixed tick like every body (entity order, unique constraint priorities), so the
same inputs replay identically; everything resets when play stops.

## 2D physics

2D games (sprites and tilemaps on the XY plane, see [2D_AND_UI.md](2D_AND_UI.md)) use a second,
independent simulation built on Box2D v3. Its components mirror the 3D ones, and it runs in the
same fixed tick right after the 3D step. A 2D body moves the entity's transform: position x/y and
the rotation around Z. z, the other rotations and the scale are left alone. Units are world units
(1 tile at `cellSize` 1).

| Component | What it is for |
|---|---|
| `collider2d` alone | Static geometry: ground, walls, platforms, tilemap collision |
| `body2d` + `collider2d` | A simulated body: `dynamic` (gravity, forces, collisions), `kinematic` (follows its transform, or its `velocity` when set; pushes dynamic bodies) or `static` |
| child entities with `collider2d` | Extra shapes of the ancestor's body (a compound) |
| `character2d` | A kinematic platformer controller: slopes, one-way platforms, coyote time, jump buffering |
| `joint2d` | `revolute`, `prismatic`, `distance`, `weld`, `wheel` or `target`, to another body (`other`, an entity link) or to the world |
| `physics2d_world` | Gravity (default `[0, -20]`), solver sub-steps, sleeping, the `impact` threshold, debug drawing |

### `collider2d`

| Field | Meaning |
|---|---|
| `shape` | `box` (`size`), `circle` (`radius`), `capsule` (`radius`, `height` along local Y), `polygon` (`points`, convex, 3..8), `chain` (`points`, >= 4, `loop`), `segment` (2 `points`), `tilemap` |
| `offset`, `rotation` | Shape placement relative to the entity (scaled and turned with its transform) |
| `friction`, `restitution`, `density` | Material: density x area gives dynamic bodies their mass (or set `body2d.mass`) |
| `sensor` | A zone: `on trigger_enter` / `on trigger_exit`, never blocks (sensors ignore static scenery) |
| `layer`, `mask` | Collision layer (the 3D names: default, static, player, enemy, projectile, trigger, debris) and the layers it collides with (`"all"` or `"default, player"`) |
| `oneWay` | A platform that only blocks from above (its local +Y): jump up through it, land on it |
| `tileMerge`, `tileShapes` | `tilemap` only: `chains` (merged outlines) or `boxes` (merged rectangles); per-tile shapes over the tileset's |

`body2d`: `motion`, `mass` (0 = from density), `gravityScale`, `linearDamping`, `angularDamping`,
`fixedRotation`, `bullet` (continuous collision against moving bodies), `allowSleep`, `startAwake`,
and the live `velocity`, `angularVelocity` (degrees/s) and `sleeping`. Writing `velocity` from a
script or a tool sets it.

### Tilemap collision

`collider2d {"shape": "tilemap"}` on an entity with a `tilemap` builds its collision from the solid
layers (`"solid": true` = every tile, `"solid": "tiles"` = the tiles in the tileset's `solid` ids,
the map's `solidTiles`, or the collision table). Full tiles are merged: with `tileMerge: "chains"`
(the default) each connected region becomes one outline loop (holes included), so bodies and
characters slide along tile seams without catching. With `"boxes"` they become the fewest merged
rectangles. Other tile shapes come from the collision table, either the tileset's `"collision"`
object or `collider2d.tileShapes`, keyed by tile id:

```json
{"image": "tiles.png", "tileSize": 16, "solid": ["1-40"],
 "collision": {"41": "slope_up", "42": "slope_down", "43": "half_bottom", "44": "top",
               "45": {"points": [[0, 16], [16, 8], [16, 16]]}, "46": "none"}}
```

Presets: `full`, `none`, `slope_up`, `slope_down`, `slope_up_low`/`slope_up_high` and
`slope_down_low`/`slope_down_high` (a gentle slope over two tiles), `half_bottom`, `half_top`, `top`
(a thin one-way platform). Polygons are in tile pixels (origin top-left, y down) and can be
one-way with `"oneWay": true`. Flipped tiles flip their shape. Half tiles and one-way tops in a row
merge into one piece. `physics2d_info` reports how many loops, boxes and polygons each map became.

### `character2d`

A capsule (`height`, `radius`, `offset`: `[0, height / 2]` puts the origin at the feet) moved by
Box2D's mover queries rather than by forces, so it never jitters, never tips over and stops
exactly where it should. Each tick it accelerates toward the input (`moveSpeed`, `acceleration`,
`airControl` in the air), applies its own `gravity` (times `fallMultiplier` while falling,
clamped at `maxFallSpeed`), slides along walls, walks up and down slopes up to `maxSlope` while
staying glued to the ground (`snapDistance`), lands on one-way platforms and passes up through
them. A jump works up to `coyoteTime` seconds after running off a ledge, and a jump pressed in the
air up to `jumpBuffer` seconds before landing happens on landing. `velocity` and `grounded` are live;
write `velocity` for knockback. A kinematic proxy body follows it, so sensors detect the character
and dynamic bodies it walks into are pushed.

### `joint2d`

`anchor` is the pivot in this entity's space, `other` the second body (empty = the world) and
`otherAnchor` its end (distance joints) in the other body's space, or a world point when there is
no `other`. Motors (`motorSpeed`, `motorForce`) move this body relative to the other one: revolute
counter-clockwise for positive speeds, prismatic and wheel along `axis`. `limitMin < limitMax`
turns limits on, `stiffness` (Hz) and `damping` make it springy, `breakForce` breaks it (`enabled`
becomes false and `on event "joint_broken"` fires). `target` pulls the body toward `target` (a world
point) or toward the `other` entity as it moves (dragging, grappling).

### Wander

The 3D events are delivered by 2D physics too, with the same names and values: `on collide`
(`other`, `contact_point`, `contact_normal`, `impact` = approach speed), `on trigger_enter` and
`on trigger_exit`. 2D adds `on collide_end` (two bodies stopped touching) and `on impact` (a hit faster
than `physics2d_world.impactSpeed`; `data.point`, `data.normal`, `data.speed`, `data.impulse`).

| Builtin | Meaning |
|---|---|
| `push2d(e, force)`, `impulse2d(e, impulse)`, `torque2d(e, n)` | force (N), instant kick (N s), torque for dynamic bodies |
| `velocity2d(e)`, `set_velocity2d(e, v)` | read or set the velocity of a body or character |
| `raycast2d(origin, dir, max?)` | first entity hit (not self, not sensors); then `hit_point`, `hit_normal`, `hit_distance` |
| `overlap2d(center, r, tag?)` | nearest entity overlapping a circle, optionally with a tag |
| `point2d(point, tag?)` | entity whose shape contains a point (sensors included) |
| `move2d(e, x)`, `jump2d(e, speed?)`, `grounded2d(e)`, `drop_through2d(e)` | character2d control |

```wander
behavior Hero
  on tick
    move2d(self, axis("move").x)
    if pressed("jump") then
      jump2d(self)
    end
    if pressed("down") then
      drop_through2d(self)
    end
  end
  on trigger_enter "coin"
    emit "coin_collected"
  end
end

behavior Bomb
  on impact
    if data.speed > 8 then
      let victim = overlap2d(self, 3, "enemy")
      if exists(victim) then
        impulse2d(victim, direction(self, victim) * 20)
      end
      destroy self
    end
  end
end
```

### Tools

| Tool | What it does |
|---|---|
| `physics2d_add` | Presets fitted to the sprite: `platformer_player`, `crate`, `ball`, `one_way_platform`, `tilemap_collision`, `static_ground`, `sensor_zone`, `moving_platform`, `remove` (one undo step) |
| `physics2d_info` | Settings, counts, bodies, characters, tilemap pieces and warnings (editing and playing) |
| `physics2d_query` | `raycast`, `raycast_all`, `overlap_circle`, `overlap_box`, `point` (works while editing) |
| `physics2d_settle` | Simulate the listed entities until they rest and keep the poses (one undo step) |

```jsonc
{"tool": "physics2d_add", "args": {"entity": "Level", "preset": "tilemap_collision"}}
{"tool": "physics2d_add", "args": {"entity": "Hero", "preset": "platformer_player"}}
{"tool": "physics2d_add", "args": {"entities": ["Crate 1", "Crate 2"], "preset": "crate"}}
{"tool": "physics2d_settle", "args": {"entities": ["Crate 1", "Crate 2"]}}
{"tool": "physics2d_info", "args": {}}
```

**Determinism.** Box2D runs single-threaded and is compiled without floating-point contraction.
Bodies are created in scene order and events are sorted, so a play session replays bit for bit, and
the world is rebuilt from the restored scene on every play.

**Debug draw.** `physics2d_world.debugDraw: true` adds every shape (colored by kind: dynamic,
sleeping, kinematic, static, sensor, one-way, character), contact normals and joint lines to the
frame (`render2d.debugLines`). Headless captures draw them over the world.

## Limitations (v0.1)

- One `joint` per entity (chain links joint the previous link); no ragdoll preset yet.
- Navmesh obstacles are baked: moving obstacles are avoided by agents (crowd avoidance) but do not
  carve the navmesh. No off-mesh links (jumps, ladders) yet.
- 3D collide events are begin-only (no "collide end"); trigger exit is reported. 2D physics has `on collide_end`.
- 2D: chain and tilemap colliders belong on static or kinematic bodies (no mass). Slopes and half
  tiles are separate polygons next to the merged outlines, so a fast dynamic body can catch on their
  seam (characters do not). One `joint2d` per entity.
- Heightmap files: `.r16`/`.raw` (16-bit) and `.hdr`; PNG heightmaps need an image decoder in core.
- Non-uniform scale under rotated parents is approximated by the product of the scales along the
  hierarchy (no shear), so a non-uniformly scaled parent with a rotated child can fit a collider
  slightly off.
- Large static levels: static geometry is re-validated every 8th tick (teleports every tick), so
  editing a static collider during play takes effect within 8 ticks.
- Vehicles: wheels are vertical suspension struts (no camber/toe/caster settings yet), wheels never collide with
  walls (the chassis collider does), the chassis should be uniformly scaled, motorcycles and tracked vehicles are not
  exposed, and the `vehicles` debug view draws on captures only. Wheel speeds can alternate between spinning and
  gripping from one tick to the next at the traction limit (Jolt's discrete tire model); the telemetry and assists
  smooth over it.

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
| `engine/include/skywalker/ecs/VehicleComponents.h` | `vehicle`, `chase_camera` |
| `src/physics/Vehicles.cpp` | Jolt vehicle constraints: wheel fitting, assists, aero, telemetry, wheel visuals, engine audio |
| `src/physics/VehiclePresets.cpp`, `VehicleControls.cpp` | presets; drive actions and the chase camera |
| `src/physics/VehicleTestDrive.cpp`, `VehicleDebugDraw.cpp` | the test-drive autopilot; the `vehicles` overlay and telemetry JSON |
| `src/agent/VehicleTools.cpp`, `src/engine/VehicleBuiltins.cpp` | tools and Wander builtins |
| `tests/test_physics.cpp`, `tests/test_nav.cpp`, `tests/test_vehicle.cpp` | tests |
| `engine/include/skywalker/ecs/Body2D.h`, `Collider2D.h`, `Joint2D.h`, `Character2D.h`, `Physics2DSettings.h` | the 2D components (tables in `physics2d/Physics2DComponents.cpp`) |
| `engine/include/skywalker/physics2d/Physics2DWorld.h`, `src/physics2d/Physics2DWorld.cpp` | Box2D world mirroring the scene: bodies, shapes, joints, events, write-back |
| `src/physics2d/Character2DController.cpp` | the character2d controller (mover queries, slopes, one-way platforms, coyote time, jump buffer) |
| `engine/include/skywalker/physics2d/TileColliders.h` | merged tilemap collision (outlines, boxes, per-tile shapes) |
| `engine/include/skywalker/physics2d/Physics2DSystem.h` | play/edit worlds, Wander events, debug draw |
| `src/physics2d/Physics2DBuiltins.cpp`, `src/agent/Physics2DTools.cpp` | Wander builtins and the `physics2d_*` tools |
| `cmake/Physics2DDeps.cmake` | Box2D v3.1.1 via FetchContent |
| `tests/test_physics2d.cpp` | 2D physics tests |
