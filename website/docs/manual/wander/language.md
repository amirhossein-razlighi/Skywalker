# Wander language

This page is the language reference for people who write Wander by hand: declarations, triggers, statements, values,
types and the rules the runtime follows. Every example on the page compiles on its own with `skywalker check`. The
functions scripts can call are listed in the generated [builtins reference](../../reference/wander.md).

## Files and declarations

A script is one or more `behavior Name ... end` blocks, or bare members, in which case the whole file is one behavior
named `Main`. Around the behaviors, a file may hold `use`, `const`, `fn` and `test` declarations that its behaviors
share.

| Declaration | Meaning |
|---|---|
| `intent "..."` | The behavior's natural-language intent. |
| `var hp = 3`, `var target: entity? = none` | Per-entity state, read as `hp` or `self.hp`. Initializers run when the behavior starts and never overwrite a value the entity already has (set in the editor, by an agent or by a prefab). |
| `param speed = 3 in 0..10 "doc"` | A var that is a *tunable*: a constant default, an optional range and a description. The editor shows a slider; the spec lists it. |
| `const MAX = 10` | A compile-time constant: literals, arithmetic and other constants. |
| `fn name(a, b: number) -> number ... end` | A function. Behavior-level functions can read and write the behavior's vars. Recursion is allowed up to a depth of 200. |
| `on <trigger> ... end` | A handler (see [Triggers](#triggers)). |
| `state Name ... end` | A state of the behavior's state machine, holding its own handlers. |
| `test "name" ... end` | An in-language test, run by `wander_test` ([Specs and tests](testing.md)). |
| `use "scripts/combat"` (`as c`) | A module: a `.wander` file of `fn` and `const` declarations in the project. |

```wander
behavior Turret
  intent "Turn toward the nearest enemy in range and fire every half second."
  param range = 12 in 2..40 "detection radius (m)"
  param rate = 2 in 0.5..10 "shots per second"
  const BARREL_HEIGHT = 1.2
  var target: entity? = none
  var cooldown = 0
  var shots = 0

  on tick
    target = nearest("enemy", range)
    cooldown -= dt
    if target then
      look self at target
      if cooldown <= 0 then
        fire()
        cooldown = 1 / rate
      end
    end
  end

  fn fire()
    shots += 1
    let muzzle = self.position + (0, BARREL_HEIGHT, 0)
    emit "shot" with {from: muzzle, at: target}
  end
end
```

### Modules

A module is a project file that holds only `fn` and `const` declarations. This one compiles on its own:

```wander
-- scripts/combat.wander
const MAX_HP = 10
const CRIT_MULTIPLIER = 2

fn damage(base: number, crit: bool) -> number
  if crit then
    return base * CRIT_MULTIPLIER
  end
  return base
end

fn clamp_hp(hp: number) -> number
  return clamp(hp, 0, MAX_HP)
end
```

A behavior imports it with `use`, by project-relative path without the extension, and calls into it through the
module name or an alias. Because it needs the module file, check it with `--project`:

```text
use "scripts/combat"
use "scripts/combat" as c

behavior Knight
  var hp = combat.MAX_HP
  on event "hit" with h
    hp = combat.clamp_hp(hp - c.damage(h.get("amount", 1), chance(0.1)))
  end
end
```

Modules reload when play starts and when their files change. Errors inside a module report the module's file.

## Triggers

Handlers run when their trigger fires. The core triggers are part of the language:

| Trigger | Fires |
|---|---|
| `on start` | Once, the first tick the behavior runs (after the var initializers). |
| `on tick` | Every fixed tick (1/60 s). `on update` is accepted as an alias. |
| `on event "name"` (`with x`) | When an event of that name is delivered. The payload is `data` (or `x`), the sender is `other`. |
| `on key "space"` | When a key is pressed. |
| `on click` | When the entity is clicked (in the editor's play mode, the player, or `sim_input` / `click` in tests). |
| `on enter`, `on exit` | Inside a `state`: when the state is entered or left. |
| `on frame` | Every displayed frame, for cosmetic effects only (see [on frame](#on-frame-cosmetic-handlers)). |

Subsystems register further trigger words:

| Trigger | Fires | Names available |
|---|---|---|
| `on action "jump"` | An input action from `input.json` was pressed ([Input](../input.md)). | |
| `on collide` (`"name or tag"`) | Two physics bodies touched. | `other`, `contact_point`, `contact_normal`, `impact` |
| `on trigger_enter` (`"name or tag"`) | Something entered this trigger collider. | `other` |
| `on trigger_exit` (`"name or tag"`) | Something left this trigger collider. | `other` |
| `on ui "Name"` | The button, toggle, slider or input named `Name` was used ([2D and UI](../2d-ui.md)). | |
| `on dialogue "end"` | A conversation event: `"start"`, `"line"`, `"choice"`, `"end"`, or a `<<command>>` of a `.dialogue` script. | |
| `on anim "footstep"` | An animation event on this entity, such as a sprite clip's frame event or `"finished"` ([Animation](../animation.md)). | |
| `on pause`, `on resume` | The game was paused or resumed with `pause_game()` / `resume_game()`. Delivered to every behavior, also those the pause stops. | |

`wander_reference` lists every trigger the running engine knows, including those added by native modules.

```wander
behavior Crate
  intent "Breaks when something hits it hard, drops a coin when the player opens it."
  var broken = false

  on collide
    if impact > 8 and not broken then
      broken = true
      burst(30)
      play_sound("audio/crate_break.wav")
      destroy self
    end
  end

  on trigger_enter "player"
    log "{other.name} is next to the crate"
  end

  on action "interact"
    if nearest("player", 2) then
      spawn("prefab:prefabs/coin.prefab.json", self.position + (0, 1, 0))
    end
  end
end
```

```wander
behavior MenuAndCutscene
  on ui "Play"
    log "starting the game"
  end

  on dialogue "end"
    emit "cutscene_done"
  end

  on anim "footstep"
    play_sound("audio/step.wav", 0.4)
  end

  on pause
    self.color = #808080
  end

  on resume
    self.color = #ffffff
  end
end
```

## Statements

| Statement | Notes |
|---|---|
| `let x = v`, `let x: number = v`, `const y = v` | Block-scoped locals. |
| `x = v`, `set x to v`, `x += v` (`-=`, `*=`, `/=`) | Assign to locals, vars, properties (`self.position.x = 1`) and elements (`items[0] = 3`, `stats.hp = 2`). |
| `if c then ... elif c then ... else ... end` | `then` is optional. |
| `while c ... end` | Bounded by the step budget. |
| `for x in list`, `for i, x in list`, `for k, v in map`, `for ch in "text"` | Iterates over a snapshot: changing the collection inside the loop does not affect the loop. |
| `for i in 0..10` (10 excluded), `for i in 1..=10`, `for i in 10..0 step -2` | Numeric ranges. |
| `repeat n times ... end`, `break`, `continue` | |
| `return v`, `stop` | `stop` (or a bare `return`) leaves a handler. |
| `every 2 seconds ... end`, `after 1 seconds ... end` | Timers that advance each time the statement runs. |
| `wait 1.5`, `wait frames 3`, `wait until hp <= 0` | Suspend the handler. Allowed in handlers and tests, not inside `fn`. |
| `go to State` | Switch the behavior's state machine. |
| `move e by v`, `move e toward p at speed`, `rotate e by (0, 90 * dt, 0)`, `look e at p` | Transforms in meters and degrees. Entities count as their world position. |
| `emit "evt"` (`with payload`) (`to entity`) | Delivered next tick, in emission order. Without `to`, the event is broadcast. |
| `destroy e`, `log value` | `destroy` is deferred to the end of the tick. `log` writes to the runtime log (`logs`). |
| `expect c ("why")`, `press "k"`, `hold "k"`, `release "k"`, `click e` | Tests only. |

```wander
behavior Loops
  var total = 0
  var names: list = []

  on start
    for i in 1..=10
      total += i
    end
    for i in 10..0 step -2
      if i == 4 then continue end
      names.push("wave {i}")
    end
    let stats = {hp: 3, armor: 1}
    for key, value in stats
      log "{key} = {value}"
    end
    let n = 0
    while n < 5
      n += 1
      if n == 3 then break end
    end
    repeat 3 times
      total -= 1
    end
    set total to total * 2
  end
end
```

```wander
behavior Timers
  var lit = true

  on tick
    every 2 seconds
      lit = not lit
      if lit then
        self.light.intensity = 3
      else
        self.light.intensity = 0
      end
    end
  end
end
```

!!! note

    `every` and `after` advance only while the statement runs. Put them in `on tick` (or in a state's `on tick`)
    so they are evaluated every tick.

## Values, operators and properties

| Kind | Literals and notes |
|---|---|
| Numbers | `3`, `1.5`, `-2` (double precision) |
| Strings | `"text"` with `{interpolation}`: `"hp: {hp}"`; `\{` for a literal brace |
| Booleans and none | `true`, `false`, `none` |
| Colors | `#rgb`, `#rrggbb`, `#rrggbbaa` |
| Vectors | `(x, y, z)`; two components give `z = 0`; four components make a color |
| Lists | `[1, 2, 3]` |
| Maps | `{hp: 3, "max hp": 5}` |
| Entities | Results of `self`, `other`, `find`, `nearest`, `spawn`, ... |

Operators: `+ - * / %`, comparisons `< <= > >= == !=`, logic `and or not`, membership `x in list`, `x in map`
(a key), `x in "string"` (a substring). `"text" + anything` joins text; `list + list` concatenates; vectors and colors
do component-wise math. `0`, `""`, `none` and empty lists and maps are false in conditions; `and` and `or` produce
booleans.

Names that are always available:

| Name | Value |
|---|---|
| `self` | The entity running the behavior |
| `other` | The sender of an event, or the other entity of a contact |
| `dt` | Seconds per tick (1/60, scaled by the time scale) |
| `time` | Game time in seconds since play started (stops while paused) |
| `frame` | The tick counter |
| `pi` | 3.14159... |
| `state`, `state_time` | The current state's name and the seconds spent in it |
| `data` | The payload of the event being handled |
| `contact_point`, `contact_normal`, `impact` | Contact data in `on collide` |
| `hit_point`, `hit_normal`, `hit_distance` | Details of the last `raycast` hit |

Entity properties: `position`, `rotation` (Euler degrees), `scale`, `color`, `name`, `id`, `enabled`, `tags`, `parent`,
`state`; components as `e.light.intensity` or `e.particles.rate`; another entity's vars as `e.hp`. Vectors have
`.x .y .z .length`, colors `.r .g .b .a`, lists and strings `.length`, maps `.key`. `self.position` is relative to the
parent; `world_position(e)` gives the world position.

```wander
behavior Beacon
  param pulse = 2 in 0.1..10 "pulses per second"

  on tick
    let k = (sin(time * pulse * 2 * pi) + 1) / 2
    self.light.intensity = lerp(1, 6, k)
    self.color = hsv(time * 0.1, 0.8, 1)
    let p = nearest("player", 20)
    if p and distance(self, p) < 3 then
      log "{p.name} is {round(distance(self, p), 1)} m away"
    end
  end
end
```

### Lists and maps are values

Lists and maps have value semantics: assignment copies (copy-on-write, so copies are cheap) and `let copy = items`
never aliases. Mutate in place with methods: `self.inventory.push(item)`. Methods that change their receiver write the
result back to where it came from, so pushing onto a var updates the var. This keeps every value serializable (vars are
JSON) and makes reference cycles impossible.

```wander
behavior Inventory
  var items: list = ["sword"]
  var stats: map = {gold: 10}

  on event "pickup" with p
    let snapshot = items
    items.push(p.get("item", "rock"))
    stats.gold = stats.get("gold", 0) + p.get("gold", 0)
    log "had {snapshot.length} items, now {items.length}"
    if "key" in items then
      emit "can_open_doors" to self
    end
  end

  on event "drop"
    if not items.is_empty() then
      let dropped = items.pop()
      log "dropped {dropped}"
    end
  end
end
```

## Types

Annotations are optional: `number`, `bool`, `string`, `vec`, `color`, `entity`, `list`, `map`, `any`, `none`, unions
such as `number|string`, and `T?` for "T or none". Without annotations, types are inferred: `let x = 3` is a number,
`var target = none` is `any`. The compiler reports an error only when a mismatch is certain (`"a" - 1`,
`distance("a", self)`, assigning a string to a `number` var, `v.w` on a vector) and stays silent when a value could be
several things. You get type errors before play without writing annotations.

```wander
fn mix(a: number, b: number, t: number) -> number
  return a + (b - a) * clamp(t, 0, 1)
end

behavior Typed
  var label: string|number = "none yet"
  var target: entity? = none

  on tick
    target = nearest("enemy")
    if target then
      label = round(mix(0, 100, distance(self, target) / 50))
    end
  end
end
```

## Runtime semantics

### Tick order

Every fixed tick, the runtime visits active entities in scene order (reordered by `process.priority`, lower first),
and for each script:

1. starts it if it is new: var initializers, `on start`, the initial state's `on enter`;
2. resumes waiting handlers whose wait elapsed;
3. delivers the events emitted during the previous tick, in emission order;
4. delivers physics contacts, key presses, input actions and clicks;
5. runs `on tick`: the behavior-level handler first, then the current state's.

Spawned entities start their behaviors on the next tick. Destroyed entities disappear at the end of the tick.

### Coroutines and wait

A handler that reaches `wait` is suspended with its locals and resumes on a later tick: `wait 0.5` is 30 ticks at
60 Hz, `wait frames 3` is 3 ticks, `wait until c` re-checks the condition once per tick. A waiting `on tick` handler does
not start again until it finishes; other handlers start a new run per trigger, up to 64 waiting runs per script.
Leaving a state cancels the waiting runs of that state's handlers. Stopping play clears everything.

```wander
behavior Door
  intent "When it receives open, slide up over a second, stay open three seconds, then close."
  var busy = false

  on event "open"
    if busy then stop end
    busy = true
    repeat 60 times
      move self by (0, 3 / 60, 0)
      wait frames 1
    end
    wait 3
    repeat 60 times
      move self by (0, -3 / 60, 0)
      wait frames 1
    end
    busy = false
  end
end
```

### State machines

The first declared state is the initial one. `go to X` ends the running handler, runs the old state's `on exit`, then
the new state's `on enter`. `state` and `state_time` read the current state; `e.state` reads another entity's. More
than 32 transitions in one tick (states bouncing back and forth) is a runtime error.

```wander
behavior Firefly
  state Idle
    on enter
      self.light.intensity = 0.5
    end
    on tick
      if state_time > 2 then go to Glow end
    end
  end

  state Glow
    on enter
      self.light.intensity = 4
    end
    on exit
      log "glowed for {round(state_time, 2)} s"
    end
    on tick
      if state_time > 0.5 then go to Idle end
    end
  end
end
```

### Vars and tools

While the game plays, vars live in fast per-entity tables and are mirrored into the entity's `vars` after every tick,
so `entity_get` and the inspector always show current values. Edits made between ticks by tools are picked up on the
next tick. Replacing a behavior while playing restarts its instance.

### Step budget and errors

Each handler run has a budget of 1,000,000 steps. Every loop iteration charges the loop body's length and every call
the callee's length; heavy builtins such as `sort` charge per element. Exceeding the budget aborts the run with an
error at the loop's line, so no script can hang the engine.

A runtime error (with its line, and its file for module code) aborts that handler run and is reported in `logs`. After
five errors the script is disabled until play restarts.

### Determinism

Randomness comes from the scene's seed (`random`, `chance`, `pick`, `shuffle`, `random_int`) and there is no
wall-clock access. Two runs of the same scene produce the same trace, in the VM and in AOT-compiled native code.

### Limits

| Limit | Value |
|---|---|
| Steps per handler run | 1,000,000 |
| Recursion depth | 200 |
| Waiting runs per script | 64 |
| State transitions per tick | 32 |
| `spawn` calls per tick | 256 |
| Entities | 20,000 |
| Runtime errors before a script is disabled | 5 |

### Pause and time scale

`pause_game()`, `resume_game()` and `time_scale(x)` apply from the next tick. While the game is paused, behaviors of
`pausable` entities (the default) stop: no `on tick`, no waits or timers advancing, no input. They still receive
`on pause` and `on resume`, and events they have handlers for wait and arrive on resume. Entities whose
`process.mode` is `always` or `when_paused` keep running, and UI canvases run `always` by default: that is where pause
menu logic goes.

`dt` is scaled by the time scale (it is real time on entities with `process.clock` set to `real`). `time` is game time
and stops while paused; `unscaled_dt()` and `unscaled_time()` are real time. See
[Simulation and time](../simulation.md).

```wander
behavior PauseMenu
  intent "Escape toggles the pause; the panel shows while paused."

  on action "pause"
    if is_paused() then
      resume_game()
    else
      pause_game()
    end
  end

  on pause
    find("PausePanel").ui.visible = true
  end

  on resume
    find("PausePanel").ui.visible = false
  end

  on ui "Resume"
    resume_game()
  end
end
```

Put this behavior on a UI canvas entity (canvases run `always`) and keep that entity enabled: a disabled entity runs
nothing, not even `on pause`.

## on frame: cosmetic handlers

`on tick` runs at the fixed 60 Hz. On a 120 Hz display the engine already shows motion between ticks (render
interpolation), but some touches are best computed per displayed frame: camera shake, bobbing, flicker, UI tweens.
`on frame` handlers run once per displayed frame while the game plays, after interpolation, with `dt` = real seconds
since the last frame (scaled by the time scale) and `time` = the displayed game time.

They are **cosmetic**: everything they write is undone after the frame, so the simulation, and every replay, test and
trace, is identical however many frames are shown. They may:

- set `position`, `rotation`, `scale`, `color` and fields of `transform`, `mesh`, `light`, `camera`, `sprite`, `text`,
  `ui` and `light2d`;
- read anything and call pure functions and read-only queries (`find`, `distance`, `key`, `axis`, `raycast`,
  `anim_state`, `noise`, `log`, ...).

They may not change vars, `wait`, use `every` or `after`, `go to`, `move`, `rotate`, `look`, `emit`, `spawn`,
`destroy`, or call `random` (it advances the seeded generator; use `noise`). The compiler reports these as
`frame_not_cosmetic` with a hint. Functions they call are checked at run time: the first violation stops that script's
frame handlers until the next play.

```wander
behavior CameraShake
  var trauma = 0

  on event "hit"
    trauma = min(1, trauma + 0.5)
  end

  on tick
    trauma = max(0, trauma - dt * 1.5)
  end

  on frame
    let k = trauma * trauma * 0.3
    let jitter = (noise(time * 40) - 0.5, noise(time * 40 + 9) - 0.5, 0)
    self.position = self.position + jitter * k
  end
end
```

The pattern is general: keep state in vars updated by `on tick`, and let `on frame` turn that state into motion.

## Idioms

| Goal | Idiom |
|---|---|
| Smooth motion toward a goal | `move self toward target at speed`, or `self.position = lerp(self.position, goal, 1 - exp(-8 * dt))` |
| Frame-rate independence | Multiply every per-tick change by `dt`. |
| Sequences of actions | `on event "open"` with `wait` between steps, no manual timers |
| Finding things | `nearest("enemy", 10)`, `find_all("coin")`, `find("Door")`; check `if x then` before using a value that can be none |
| Cooldowns | `var cooldown = 0`, `cooldown -= dt`, `if cooldown <= 0 then ... cooldown = 1 end` |
| Modes | States (`Idle`, `Chase`, `Flee`) rather than boolean flags |
| Shared code | A module with `fn` and `const`, imported with `use` |
| Teleports | `teleport(self, checkpoint)` so render interpolation does not smear the jump |

Conventions: meters, +Y up, entities face −Z, rotations are Euler degrees (pitch X, yaw Y, roll Z).

```wander
behavior Chaser
  intent "Chase the player at 4 m/s, dash every 3 seconds, respawn at the start when it falls."
  param speed = 4 in 0..12 "chase speed (m/s)"
  var dash_cooldown = 0
  var home = (0, 0, 0)

  on start
    home = self.position
  end

  on tick
    let p = nearest("player", 30)
    if not p then stop end
    move self toward p at speed
    dash_cooldown -= dt
    if dash_cooldown <= 0 and distance(self, p) < 6 then
      move self by direction(self, p) * 2
      dash_cooldown = 3
    end
    if self.position.y < -20 then
      teleport(self, home)
    end
  end
end
```

## Pitfalls

- An event emitted this tick arrives next tick. A handler that emits and immediately reads the receiver's state sees
  the old state.
- `self.position` is local to the parent. Compare world positions with `distance(a, b)`, which accepts entities.
- Builtins that may return `none` (`nearest`, `find`, `raycast`, `overlap_sphere`) must be checked before use: reading a
  property of `none` is a runtime error.
- Lists are copied on assignment: `let l = self.items; l.push(x)` changes the copy, not the var. Push on the var.
- `random` inside `on frame` is a compile error; use `noise(time * k)`.

!!! agent "For agents"

    Compile every draft before you attach it, and ask for the canonical formatting so diffs stay small:

    ```tool
    wander_reference {"topic": "list"}                     # methods and signatures of one category
    wander_check {"source": "behavior Bob\n  on tick\n    self.position.y = 1 + sin(time) * 0.2\n  end\nend\n", "format": true}
    wander_inspect {"entity": "Guard"}                      # while playing: current state, waiting handlers, vars
    logs {"limit": 30}                                      # runtime errors with line numbers
    ```

## Reference

- [Wander builtins](../../reference/wander.md): every function, method and subsystem trigger, generated from the
  registry
- Tools: [`wander_reference`](../../reference/tools/wander.md#wander_reference),
  [`wander_check`](../../reference/tools/wander.md#wander_check),
  [`wander_inspect`](../../reference/tools/wander.md#wander_inspect),
  [`logs`](../../reference/tools/sim.md#logs)
- Components: [`process`](../../reference/components/core.md#process) (pause modes, priority, clock)
- Design document: [docs/WANDER.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/WANDER.md)
