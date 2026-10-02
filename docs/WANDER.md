# Wander & ECPS

**ECPS (Entity · Component · Prompt System)** extends ECS with a third pillar: the *prompt*.

- **Entities** have **components** (data: transform, mesh, light, camera, …).
- They also have **behaviors**. Each behavior is an **intent**, a natural-language
  description written by a person or an agent, paired with its **Wander** code, a small
  deterministic program the engine runs.

The intent is the source of truth for humans. The code is the source of truth for the
runtime. Each can be edited independently:
- **Weave** asks the gameplay agent to (re)write the code from the intent, check it with the
  compiler, and verify it in simulation.
- Agents that change code are expected to keep the intent accurate.

## Why a new language?

Language models write it reliably, and anything they write is safe to run:

- **Keyword-led statements** that read like the intent: `move self toward goal at 3`.
- **Guaranteed termination.** There are no `while` loops, only `repeat N times` with
  N ≤ 1000, plus a per-handler evaluation budget.
- **Determinism.** Fixed ticks, seeded `random()`, ordered event delivery: runs replay exactly.
- **Precise diagnostics.** Every error has a line, a column, a stable code, and usually a
  hint (`unknown function 'distnace' — did you mean 'distance'?`).
- **Live checking.** `wander_check` compiles without attaching. `behavior_set` refuses code
  with errors unless `allow_errors` is set.

## Example

```wander
behavior Patrol
  intent "Walk back and forth; turn red when the player is close."
  var speed = 2

  on tick
    move self by (sin(time) * speed * dt, 0, 0)
    let player = nearest("player")
    if exists(player) and distance(self, player) < 3 then
      self.color = #ff4040
    else
      set self.color to #40c0ff
    end
    every 2 seconds
      emit "patrol_ping"
    end
  end

  on event "alarm"
    speed = speed * 2
  end
end
```

## Reference

| Category | Syntax |
|---|---|
| Triggers | `on start`, `on tick`, `on event "name"`, `on key "space"`, `on click` |
| Variables | `var name = value` (per-entity, persisted in `vars`; use `name` or `self.name`), `let x = v` (local) |
| Assignment | `target = value` or `set target to value` |
| Control flow | `if … then … elif … then … else … end`, `every <sec> … end`, `after <sec> … end`, `repeat <n> times … end`, `stop` |
| Actions | `move e by vec`, `move e toward point\|entity at speed`, `rotate e by degrees`, `look e at point\|entity`, `emit "evt" (to e)`, `destroy e`, `log value` |
| Values | numbers, `"strings"`, `true/false`, `none`, colors `#rgb/#rrggbb/#rrggbbaa`, vectors `(x, y, z)` |
| Operators | `+ - * / %`, `< <= > >= == !=`, `and or not`; `"text" + anything` concatenates |
| Built-ins | `self`, `dt`, `time`, `frame`, `pi` |
| Properties | `e.position e.rotation e.scale e.color e.name e.id e.enabled`, `e.<component>.<field>`, `e.<var>`, `.x/.y/.z`, `.r/.g/.b/.a` |
| Functions | `find nearest count tagged exists spawn distance direction forward length normalize dot cross vec color sin cos tan abs sqrt floor ceil round sign min max clamp lerp random chance key str` |
| Comments | `-- …`, `// …`, `# …` (a `#` followed by a space) |

Conventions: meters; +Y up; entities face −Z; rotations are Euler degrees `[pitch, yaw, roll]`.

## Runtime semantics

- `on start` runs once, the first tick a behavior is active. `var` initializers run first
  and don't overwrite values already present in the entity's `vars`.
- Each tick, the runtime visits active entities in scene order. For each one it delivers
  events emitted *last* tick, then key presses, then clicks, then runs `on tick`.
- `destroy` is deferred to the end of the tick. `spawn` takes effect immediately; the new
  entity's behaviors start next tick. Limits: 256 spawns per tick, 20,000 entities.
- A runtime error aborts that handler invocation and is reported. After 5 errors the
  script is disabled.
- Replacing a behavior while playing restarts its instance state.
