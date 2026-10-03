---
name: skywalker-wander
description: "Write gameplay behaviors for Skywalker in Wander, the engine's deterministic behavior language (intent plus code per entity) - wander_reference, wander_check, behavior_set, sim_control step, sim_trace, logs. Use whenever an entity needs scripted behavior, game rules, AI, input handling, spawning or events."
---

> **STATUS: STUB (minimal, stable workflow only).** Wander is being rewritten (a new language version with a bytecode VM and a builtin
> registry). The authoritative syntax is always the live `wander_reference` tool output and `docs/WANDER.md`. This skill deliberately
> contains only the workflow that survives the rewrite. <!-- TODO(lead): after the Wander 2 merge, fill in the new syntax highlights,
> idioms and pitfalls from docs/WANDER.md; keep every example validated by `wander_check`. -->

# Wander behaviors

An entity **behavior** is an `intent` (natural-language description, the source of truth for humans) plus Wander `source` (what the runtime
executes). Keep the intent accurate whenever you change the code. Wander is built so models write it reliably: keyword-led statements, guaranteed
termination (no `while`; bounded `repeat`), deterministic ticks (1/60 s, seeded `random()`), and diagnostics with line, column, code and a hint.

## The workflow (stable across versions)

1. **Read the reference once per session:** `wander_reference {}`. Do not write Wander from memory; the language is new and your training data
   does not contain it.
2. **Draft** the behavior and check it without attaching: `wander_check {source}` returns diagnostics (fix every error; read the hints).
3. **Attach:** `behavior_set {entity, name, intent, source}`. Code with errors is rejected unless `allow_errors:true` (do not use it to ship).
4. **Run it:** `sim_control {action:"step", ticks:120}` (deterministic), drive input with `sim_input`, read `logs`.
5. **Verify numerically:** `sim_trace {entities:[...], properties:["transform.position","vars.score"], ticks:300, every:20}` instead of eyeballing.
6. **Look:** `viewport_capture` mid-run if it is visual. `sim_control {action:"stop"}` restores the scene.
7. Remove with `behavior_remove {entity, name}`.

## Facts that are unlikely to change

- Coordinates: meters, +Y up, entities face -Z, rotations are degrees `[pitch, yaw, roll]`. Colors `#rrggbb`.
- Triggers include `on start`, `on tick`, `on event "name"`, `on key "k"`, `on click`, `on action "name"` (input actions, see skywalker-audio).
- Per-entity state lives in `var`s (persisted with the entity); `self` is the entity; component fields are reachable as `self.<component>.<field>`.
- Events connect behaviors and the studio: emit `death`, `fail`, `damage`, `objective`, `checkpoint`, `pickup` so playtest bots can measure the game
  (see skywalker-studio). Tag entities `player`, `goal`, `hazard`, `enemy` for the same reason.
- Runtime errors abort that handler; five errors disable the script. Check `logs` after every run.
- Replacing a behavior while playing restarts its instance state.

## Pitfalls

- Writing from memory instead of `wander_reference`.
- Editing a behavior during play and expecting the change to survive `stop` (play changes are restored on stop).
- Verifying by reading the code instead of running it for N ticks.
- Forgetting to emit events/tags, which blinds `playtest_run`.
