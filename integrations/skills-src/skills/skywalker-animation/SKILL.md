---
name: skywalker-animation
description: Animation in Skywalker - skeletal animation, clips, state machines, blending, IK and tweens. STUB until the animation workstream is merged; read docs/ANIMATION.md and the live tool descriptions.
---

> **STATUS: STUB.** The animation subsystem is under construction by another workstream and is not part of this build of the skill.
> <!-- TODO(lead): after merging the animation workstream, replace this body with: components (skeleton, animator, clip, state machine), tools and arguments
> (verify against `skywalker tools --markdown`), importing animated glTF (and what dcc_edit_asset does not carry), retargeting, blending recipes,
> verification (sim_trace on bone transforms, captures at fixed ticks), pitfalls. Source of truth: docs/ANIMATION.md. -->

## What to do today

1. `engine_info` / `component_schema`: look for animator, skeleton, clip or tween components and `anim_*` tools; if present read `skywalker://docs/ANIMATION`.
2. Until then, motion is behavior-driven: Wander can move, rotate and `lerp` any property each tick (see skywalker-wander). Verify with `sim_trace`
   (positions over time) and by capturing at fixed ticks (`sim_control step` then `viewport_capture`).
3. Animated models: import still works as static meshes; skeletal data and animations from `.glb` are not driven yet (check the docs for the current status).
   `dcc_edit_asset` does not carry animation or shape keys through its round trip.

## Rules that will hold

- Judge animation by sampling over time, never from a single frame: step N ticks, capture, compare.
- Keep the simulation deterministic: animation state must replay identically from a play snapshot.
