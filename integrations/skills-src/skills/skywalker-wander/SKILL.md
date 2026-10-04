---
name: skywalker-wander
description: Write, test and debug gameplay behaviors in Skywalker's Wander 2 language - intent plus spec plus code with in-language tests (wander_reference, wander_check, behavior_set, wander_test, behavior_spec), states, coroutines, events, modules, the node-graph view, runtime inspection, and native speedups (AOT compile and C++ modules). Use whenever an entity needs scripted behavior, game rules, AI, input handling, spawning or events.
---

# Wander behaviors

Load skywalker-core first. Engine doc: `skywalker://docs/WANDER`. An entity **behavior** is an `intent` (plain language, the human source of truth), an optional **spec** (rules, each verified by tests), and Wander **source**
(what the runtime executes). Wander is built so models write it reliably: keyword-led statements, guaranteed termination (a step budget instead of unbounded loops), deterministic ticks (1/60 s, seeded randomness),
diagnostics with line, column, stable code and a hint. The language is new and absent from your training data: **read `wander_reference` before writing any**.

## The workflow: intent, spec, code, tests, run

1. **Reference once per session**: `wander_reference {}`; for details `wander_reference {topic:"physics"}` (a category) or `{topic:"raycast"}` (a function).
2. **Intent**: one or two plain sentences of what the entity does. Keep it accurate when the code changes.
3. **Spec**: derive rules from the intent; each rule names the tests that verify it.
4. **Code with a `test` block per rule**, checked without attaching: `wander_check {source}` (diagnostics; add `format:true` for canonical formatting).
5. **Attach**: `behavior_set {entity, name, intent, spec, source}`. Code with errors is rejected and the diagnostics returned (`allow_errors:true` only for work in progress, never to ship).
6. **Prove it**: `wander_test {entity, name}` runs the `test` blocks in a sandbox (the real scene is untouched). `behavior_spec {entity, name}` shows rules without tests and tests without rules.
7. **Run it in the world**: `sim_control {action:"step", ticks:120}`, drive input with `sim_input`, read `logs`, check numbers with `sim_trace`, then `viewport_capture` mid-run. `sim_control {action:"stop"}` restores the scene.
8. Remove with `behavior_remove {entity, name}`.

```text
wander_reference {}
wander_check {source:"behavior Coin\n  param spin = 90 in 0..360 \"degrees per second\"\n  on tick\n    rotate self by (0, spin * dt, 0)\n  end\n  test \"spins\"\n    let before = self.rotation.y\n    wait 0.5\n    expect self.rotation.y != before\n  end\nend", format:true}
behavior_set {entity:"Coin", name:"Coin", intent:"Spins; when the player touches it, it adds 1 to the score and disappears.", spec:{summary:"Spinning pickup", rules:[{text:"It spins", tests:["spins"]}, {text:"It disappears when touched", tests:[]}]}, source:"behavior Coin\n  param spin = 90 in 0..360 \"degrees per second\"\n  on tick\n    rotate self by (0, spin * dt, 0)\n  end\n  on trigger_enter \"player\"\n    emit \"score\" with {\"points\": 1}\n    destroy self\n  end\n  test \"spins\"\n    let before = self.rotation.y\n    wait 0.5\n    expect self.rotation.y != before\n  end\nend"}
wander_test {entity:"Coin", name:"Coin", mode:"entity"}
behavior_spec {entity:"Coin", name:"Coin"}
sim_control {action:"step", ticks:120}
sim_trace {entities:["Coin"], properties:["transform.rotation"], ticks:120, every:30}
logs {limit:20}
sim_control {action:"stop"}
```

(The second rule above has no test yet: `behavior_spec` reports it as uncovered, which is the cue to add one or to cover it by a scripted playtest.)

`wander_test` modes: `entity` (default: a copy of the entity's components, tags and vars), `scene` (a copy of the whole scene), `isolated` (plain entity). Pass `source` instead of `entity`+`name` to test unsaved code, `filter` to run some tests.
Inside a test: `wait`, `wait frames n`, `emit`, `press`, `hold`, `release`, `click e`, `spawn(...)`, `expect cond ("why")`. Failures report the compared values.

## Language essentials

```wander
behavior Guard
  intent "Patrol between two posts; chase the player when close; lose 1 hp per hit."
  param speed = 3 in 0..10 "walk speed (m/s)"     -- tunable: a slider in the editor, listed in the spec
  var hp = 3                                      -- per-entity state, visible to tools, never overwritten by initializers
  var posts: list = []

  on start
    posts = [self.position, self.position + (6, 0, 0)]
  end

  state Patrol                                    -- the first state is the initial one
    on tick
      move self toward posts[floor(state_time / 4) % 2] at speed
      if nearest("player", 5) then go to Chase end
    end
  end
  state Chase
    on enter
      self.color = #ff4040
    end
    on tick
      let p = nearest("player", 8)
      if not p then go to Patrol end
      move self toward p at speed * 1.5
    end
  end

  on event "damage" with hit                       -- payload map; `other` is the sender
    hp -= hit.get("amount", 1)
    if hp <= 0 then
      emit "guard_down" with {at: self.position}
      destroy self
    end
  end

  test "loses hp when hit"
    emit "damage" with {amount: 1} to self
    wait frames 2
    expect hp == 2
  end
end
```

- **Triggers**: `on start`, `on tick`, `on event "name" (with x)`, `on key "space"`, `on action "jump"` (input actions, see skywalker-audio), `on click`, `on collide/trigger_enter/trigger_exit ("name or tag")`, `on anim "name"`, `on ui "Button"`,
  `on dialogue "cmd"`, `on pause` / `on resume` (the game was paused / resumed), `on frame` (every displayed frame, cosmetic only: see below);
  inside a `state`: `on enter`, `on exit`, `on tick`. `wander_reference` lists subsystem triggers.
- **Declarations**: `var`, `param x = 1 in 0..5 "doc"`, `const MAX = 10`, `fn name(a, b: number) -> number ... end` (recursion ok), `use "scripts/combat"` then `combat.damage(...)` (modules are `.wander` files of `fn`/`const`).
- **Statements**: `let`, `x = v`, `x += v`, `if/elif/else/end`, `for x in list`, `for i in 0..10` (end excluded; `0..=10` includes), `while` and `repeat n times` (bounded by the budget), `break`, `continue`, `return`, `stop`,
  `every 2 seconds ... end`, `after 1 seconds ... end`, `wait 1.5` / `wait frames 3` / `wait until cond` (coroutines; handlers and tests only, not inside `fn`), `go to State`, `move e by v` / `move e toward p at speed`,
  `rotate e by (...)`, `look e at p`, `emit "evt" with payload to e`, `destroy e`, `log value`.
- **Values**: numbers, `"text {interpolation}"`, `true/false/none`, colors `#ff8800`, vectors `(x, y, z)`, lists `[..]`, maps `{k: v}`. Lists and maps are **values** (assignment copies); mutate with methods: `self.items.push(x)`.
  `0`, `""`, `none` and empty collections are false. Optional types: `var target: entity? = none`.
- **Names**: `self`, `other`, `dt`, `time`, `frame`, `state`, `state_time`, `data`. Properties: `e.position e.rotation e.scale e.color e.name e.tags e.enabled`, components `e.light.intensity`, vars `e.hp`.
- Reserved words cannot be names: `fn return for in while do break continue wait until state go goto const test expect use with param other step`.
- **Idioms**: always multiply per-tick change by `dt`; smooth follow `self.position = lerp(self.position, goal, 1 - exp(-8 * dt))`; prefer `state` over boolean flags; cooldown `var cd = 0` / `cd -= dt`;
  find things with `nearest("enemy", 10)`, `find_all("coin")`, `find("Door")` and test `if x then` before using a maybe-none; inside a prefab, mark parts unique (`entity_update {entity, unique: true}`) and use `find("%Muzzle")` so each copy finds its own part; share code with `use`.

**Pause, slow motion, smoothing.** `pause_game()` / `resume_game()` / `is_paused()`, `time_scale(0.3)` (slow motion; `dt`, timers and
waits scale), `unscaled_dt()` / `unscaled_time()` (real time), `teleport(self, find("Spawn"))` (jump without a render smear). All apply from
the next tick. While paused, `pausable` entities (the default) stop: no `on tick`, waits and timers hold, events wait until resume; put pause
menu logic on a UI canvas (runs `always`) or an entity with `process: {mode: "always"}` (or `"when_paused"`). `time` is game time (stops
while paused). `process.priority` orders behaviors (lower first). `on frame` handlers are cosmetic (camera shake, bobbing, UI tweens): they
may set position/rotation/scale/color and fields of transform, mesh, light, camera, sprite, text, ui, read anything and call pure
functions; their writes are undone after each frame; vars, `wait`, `emit`, `spawn`, `random`, `move` there are compile errors
(`frame_not_cosmetic`).

```wander
on action "pause"                -- on the pause menu canvas
  if is_paused() then resume_game() else pause_game() end
end
on frame                         -- camera shake that never touches the simulation
  self.position = self.position + (noise(time * 40) - 0.5, noise(time * 40 + 9) - 0.5, 0) * self.trauma * 0.3
end
```

Runtime facts: events arrive next tick in emission order; a handler error aborts that run (logged), **five errors disable the script**; each handler run has a 1,000,000-step budget; 256 `spawn`s per tick, 20,000 entities;
vars are mirrored into `entity_get` after every tick; replacing a behavior while playing restarts its instance; changes made during play are restored on stop.

## Debugging a running behavior

`wander_inspect {entity}` (while playing): current state and its age for each state machine, handlers waiting in `wait`, and vars. Then `logs`, `sim_trace`, `sim_control step` a few ticks at a time.
Emit events and tags that `playtest_run` understands (`death`, `fail`, `damage`, `objective`, `checkpoint`, `pickup`; tags `player goal hazard enemy`) so bots can measure the game (skywalker-studio).

## Graph view and specs

`behavior_graph {entity, name, palette:true}` returns a visual node graph (`format: wander-graph`): bodies for each handler/fn/test/state, exec-flow nodes wired by exec pins, data nodes for expressions, inline literal pins, saved layout.
Edit the JSON (add a node, rewire a pin) and send it back as `graph` to `behavior_from_graph {entity, name, graph}`: it generates Wander code, compiles it (rejected on errors unless `allow_errors`), saves it and stores the node positions.
The round trip is lossless; use the graph when a human wants to see or edit flow visually, text for everything else. Pass `source` instead of `entity`+`name` to graph unsaved code.

```text
behavior_graph {entity:"Coin", name:"Coin"}
behavior_spec {entity:"Coin", name:"Coin"}
```

## Native speed (only after measuring)

Wander runs on a bytecode VM that is fast for gameplay. Reach for native code only for hot inner loops (flocking, procedural generation, thousands of entities per tick) after `perf_stats` (or the repo's wander bench tool) shows a need.

- **AOT**: `wander_compile_native {entity}` (or no argument for every behavior) translates bytecode to C++, builds it with the system `clang++`, caches it by program hash and loads it. Results, errors and step budget are identical to the VM;
  failures leave the behavior on the VM. `auto:true` compiles at every play start; `force:true` rebuilds. Edited behaviors run in the VM until compiled again. AOT libraries are **not shipped** in built games (the player runs the VM).
- **C++ modules**: `native_template {name:"gameplay"}` writes `native/gameplay.cpp` (a builtin `wave(t, freq)` and a per-tick system for entities tagged `spinner`) and `native/module.json` (flags, include_dirs, libs, frameworks, pkg_config).
  `native_build {}` compiles and loads it (compiler errors come back as `{file, line, column, message}`); its builtins appear in `wander_reference`, its systems run every tick after behaviors. `native_list {}` shows sources, compiler, last build and loaded builtins.
  Modules rebuild automatically when play starts and are shipped (compiled) by `game_build`.
- These are **trusted local code** (they run in the engine process): the human approves the tools; review the C++ you write, keep it deterministic (use the SDK's seeded randomness, no wall clock).

```text
wander_compile_native {entity:"Swarm", force:false}
native_template {name:"gameplay", overwrite:false}      # errors if the file exists; overwrite:true replaces it
native_build {force:true}
native_list {}
```

## Verification checklist

- `wander_check` clean, `wander_test` green, `behavior_spec` shows every rule covered.
- Ran N ticks with the real input (`sim_input` then `sim_control step`) and `logs` is empty of errors.
- Numbers confirmed with `sim_trace` (position, `vars.score`), a capture taken mid-run.
- Events and tags emitted for playtest bots; intent text still matches the code.

## Pitfalls

- Writing from memory instead of `wander_reference` (wrong keywords, `while` loops assumed unbounded, missing `end`).
- Editing a behavior during play and expecting the change to survive `stop`.
- Verifying by reading code instead of running it for N ticks.
- A `wait` inside an `fn` (not allowed) or an `on tick` handler that waits (it does not restart until it finishes).
- Using a reserved word as a var name (`wait`, `state`, `step`, `param`, `other`).
- Forgetting `dt`: motion that depends on frame rate. Forgetting to emit events/tags, which blinds `playtest_run`.
- Lists/maps are copied on assignment: mutate through the owner (`self.items.push(x)`), not through a copy.
