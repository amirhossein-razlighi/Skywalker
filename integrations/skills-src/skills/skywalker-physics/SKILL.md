---
name: skywalker-physics
description: Physics and navigation in Skywalker - rigid bodies, colliders, joints, character controllers, raycasts and navmesh pathfinding. STUB until the physics workstream is merged; read docs/PHYSICS.md and the live tool descriptions.
---

> **STATUS: STUB.** The physics / navigation subsystem is under construction by another workstream and is not part of this build of the skill.
> <!-- TODO(lead): after merging the physics/navigation workstream, replace this body with: components and their fields (rigid body, collider, joint,
> character controller, navmesh agent), the tool names and arguments (verify against `skywalker tools --markdown`), recipes (falling crates, a
> controllable character, a door hinge, patrolling AI on a navmesh), debugging views, determinism rules, and pitfalls. Source of truth: docs/PHYSICS.md. -->

## What to do today

1. Check whether physics exists in your build: `engine_info` lists component types and tool categories. Look for physics/collider/rigidbody
   components and `physics_*` / `nav_*` tools. `component_schema` shows fields and ranges for any component that exists.
2. If it exists, read the MCP resource `skywalker://docs/PHYSICS` (or `docs/PHYSICS.md`) before using it.
3. If it does not exist yet, fall back to what is available and verify with simulation:
   - Line of sight and ground contact: `raycast`. Resting objects on terrain/meshes: `place_on_surface`.
   - Simple motion in Wander (`move`, `look`, `lerp`) checked with `sim_trace`.
   - Deterministic by design: fixed 1/60 s ticks, play snapshot/restore on stop.

## Rules that will hold once physics lands

- Simulation is deterministic: the same inputs replay identically. Verify with `sim_control step` and `sim_trace`, not by eye.
- Measure first (`scene_overview`, `raycast`), then place; interpenetrating colliders at start explode.
- Playtest bots (`playtest_run`) steer without a navmesh today; navigation will improve them.
