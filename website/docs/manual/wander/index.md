# Wander scripting

Wander is Skywalker's behavior language. You use it to give entities game logic: a guard that patrols, a door that
opens, a coin that spins and scores. Every behavior pairs a plain-language **intent** with deterministic Wander code,
so people and AI agents can read what an entity is meant to do and verify that the code does it.

<figure markdown>
![The hello_sky example with entity ids drawn over the viewport](../../assets/images/agents/annotated-capture.webp){ loading=lazy }
<figcaption>The hello_sky example as an agent sees it (<code>viewport_capture</code> with <code>annotate: true</code>): the hero, the coin and the crystal each run a small Wander behavior.</figcaption>
</figure>

## Concepts

### Entity, component, prompt

Skywalker organizes a game as **ECPS**: Entity, Component, Prompt.

| Pillar | What it holds | Example |
|---|---|---|
| Entity | An identity in the scene: name, id, tags, parent | `Guard #12`, tagged `enemy` |
| Component | Data: transform, mesh, light, body, audio, ... | `light.intensity = 4` |
| Prompt | Behaviors: what the entity does, as intent plus code | "Patrol between two posts; chase the player when close." |

Components describe what an entity *is*; behaviors describe what it *does*. An entity can carry any number of
behaviors, and each behavior is three things:

| Part | Written by | Role |
|---|---|---|
| **Intent** | A person or an agent | The source of truth for humans: one or two sentences in plain language |
| **Spec** (optional) | Usually an agent | Rules derived from the intent, each naming the tests that verify it |
| **Code** | A person or an agent | The Wander program the engine runs: the source of truth for the runtime |

The spec and the code's `test` blocks connect the intent to the code. An agent derives rules from the intent, writes
code with one test per rule, and proves the code meets the intent with `wander_test` before anyone presses play. See
[Specs and tests](testing.md).

### Why the language looks the way it does

Wander is designed to be written correctly on the first try, by people and by language models, and to be safe to run
whatever it contains.

| Property | What it means for you |
|---|---|
| Keyword-led statements | `move self toward goal at 3`, `emit "open" to door`, `go to Chase`: code reads like the intent it implements, and models write it reliably. |
| Guaranteed termination | Every handler run has a step budget. A buggy or adversarial loop is aborted with an error at its line; it never hangs the engine. |
| Determinism | Fixed 1/60 s ticks, seeded randomness, events delivered in emission order, no wall-clock access. The same scene replays exactly, in tests, traces and movie renders. |
| Precise diagnostics | Every error has a line, a column, a stable `snake_case` code and a did-you-mean hint. |
| Gradual types | Annotations are optional; the compiler infers types and reports only mismatches that are certain. |
| Sandboxed | Scripts cannot touch files, the network or the clock. Native code is a separate, explicit opt-in ([Graphs and native code](native.md)). |

Under the hood a typed compiler turns source into register bytecode for a small virtual machine, and hot behaviors can
be compiled ahead of time to native code with identical results.

## A guided tour

The behavior below is complete: it compiles with `skywalker check` and its tests pass in `wander_test`. Read it top to
bottom; the numbered notes follow.

```wander
behavior Guard
  intent "Patrol between two posts; chase the player when close; lose 1 hp per hit."
  param speed = 3 in 0..10 "walk speed (m/s)"
  var hp = 3
  var posts: list = []

  on start
    posts = [self.position, self.position + (6, 0, 0)]
  end

  state Patrol
    on tick
      let goal = posts[floor(state_time / 4) % 2]
      move self toward goal at speed
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

  on event "damage" with hit
    hp -= hit.get("amount", 1)
    if hp <= 0 then
      emit "guard_down" with {at: self.position}
      destroy self
    end
  end

  test "starts on patrol"
    expect state == "Patrol"
  end

  test "loses hp when hit"
    emit "damage" with {amount: 1} to self
    wait frames 2
    expect hp == 2
  end

  test "is destroyed after three hits"
    repeat 3 times
      emit "damage" with {amount: 1} to self
    end
    wait frames 2
    expect not exists(self) "the guard is gone"
  end
end
```

1. **`behavior Guard ... end`** names the behavior. A file can hold several behaviors; a file of bare members
   (`on tick ... end` with no `behavior` header) is one behavior named `Main`.
2. **`intent`** stores the plain-language goal with the code. Tools and the editor show it next to the code.
3. **`param speed = 3 in 0..10 "..."`** declares a *tunable*: a variable with a constant default, a range and a
   description. The editor shows it as a slider and the spec lists it.
4. **`var hp = 3`** declares per-entity state. Initializers never overwrite a value the entity already has, so a
   designer can set `hp` to 10 on one guard in the inspector. While the game plays, vars are mirrored into the entity
   after every tick, so `entity_get` always shows current values.
5. **`on start`** runs once, the first tick the behavior runs.
6. **`state Patrol`** and **`state Chase`** form a state machine. The first state is the initial one; `go to Chase`
   switches, running `on exit` of the old state and `on enter` of the new one. `state_time` is the time spent in the
   current state.
7. **`move self toward goal at speed`** is a transform statement. Distances are meters, speeds meters per second, and
   the statement scales by the tick length for you.
8. **`nearest("player", 5)`** is a builtin: the nearest entity tagged `player` within 5 m, or `none`. Every builtin is
   listed in the generated [builtins reference](../../reference/wander.md).
9. **`on event "damage" with hit`** receives an event and names its payload `hit` (a map). `other` is the sender.
10. **`emit ... with {...}`** sends an event with a payload; it arrives next tick. **`destroy self`** takes effect at
    the end of the tick.
11. **`test "..."`** blocks run only in the sandbox of `wander_test`: `emit` injects events, `wait` advances simulated
    time, `expect` checks a condition and reports the compared values when it fails.

The full grammar is in [Language](language.md).

## How behaviors attach to entities

A behavior lives on an entity, in the scene file, next to its components. Each entry stores the name, the intent, the
optional spec, the source and whether it is enabled:

```json
{
  "name": "Crystal",
  "behaviors": [
    {
      "name": "Hover",
      "intent": "Float gently up and down and slowly spin, like it is alive.",
      "source": "behavior Hover\n  intent \"Float gently up and down and slowly spin, like it is alive.\"\n  var base = 1.6\n  on tick\n    rotate self by (0, 40 * dt, 0)\n    self.position.y = base + sin(time * 2) * 0.25\n  end\nend\n",
      "enabled": true
    }
  ]
}
```

You rarely edit this JSON by hand. You attach behaviors in one of three ways:

=== "Tool call"

    ```tool
    behavior_set {"entity": "Crystal", "name": "Hover", "intent": "Float gently up and down and slowly spin.", "source": "on tick\n  rotate self by (0, 40 * dt, 0)\n  self.position.y = 1.6 + sin(time * 2) * 0.25\nend\n"}
    ```

    `behavior_set` compiles the source first and rejects code with errors, returning the diagnostics so you can fix
    and retry. Pass `allow_errors: true` only to save work in progress.

=== "Editor"

    Select the entity, open the **Details** panel and click **Add Behavior**. Each behavior card has an **Intent**
    field, the tunable **Parameters** as sliders, and a **Code | Graph** switch: the same behavior edited as Wander
    text or as a node graph. **Weave** asks the crew's gameplay programmer to write the code from the intent, with a
    test per rule. Edits are saved with **Save**; **Revert** returns to the engine's copy.

=== "Wander"

    ```wander
    on tick
      rotate self by (0, 40 * dt, 0)
      self.position.y = 1.6 + sin(time * 2) * 0.25
    end
    ```

    A file of bare handlers is one behavior. Check it from the command line before you attach it:
    `skywalker check hover.wander`.

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/wander-behavior-card.webp · The Details panel with a behavior card: Intent field, Parameters sliders, the Code | Graph switch and the Weave button</div>

Prefabs carry behaviors too: a prefab saves its entity tree with components, behaviors, vars and tags. Because var
initializers never overwrite an existing value, a var set on one instance wins over the behavior's default.

## The workflow for agents

Agents follow one pipeline from intent to verified code. Every step is a tool call; nothing requires pressing play.

| Step | Tool | Why |
|---|---|---|
| 1. Learn the language | `wander_reference` | The guide plus every builtin with its signature, generated from the registry (including native-module builtins). Read once per session; `topic` narrows it. |
| 2. Derive a spec | (thinking) | Turn the intent into checkable rules, one sentence each, each naming a test. |
| 3. Write code with tests | (thinking) | One `test` block per rule. |
| 4. Compile | `wander_check` | Diagnostics with line, column, code and hint; a summary of vars, params, handlers, states and tests. |
| 5. Attach | `behavior_set` | Saves intent, spec and code; rejects code with errors. |
| 6. Verify | `wander_test` | Runs every test in a sandbox world; failures report the compared values. |
| 7. Check coverage | `behavior_spec` | Rules without tests, tests without rules, and the tunables with their current values. |

```tool
wander_reference {}
wander_reference {"topic": "physics"}
wander_check {"source": "on tick\n  rotate self by (0, 90 * dt, 0)\nend\n"}
behavior_set {"entity": "Coin", "name": "Spin", "intent": "Spin a quarter turn per second.", "spec": {"summary": "A spinning coin.", "rules": [{"text": "Turns 90 degrees per second around Y.", "tests": ["spins"]}]}, "source": "on tick\n  rotate self by (0, 90 * dt, 0)\nend\ntest \"spins\"\n  wait 1\n  expect self.rotation.y > 80\nend\n"}
wander_test {"entity": "Coin", "name": "Spin"}
behavior_spec {"entity": "Coin", "name": "Spin"}
```

After the tests pass, watch the behavior in the real scene: `sim_control` with `step`, `sim_input` for keys and
events, `sim_trace` for numbers over time, `logs` for `log` output and runtime errors, and `wander_inspect` for the
live state of state machines and waiting handlers. [Specs and tests](testing.md) walks through the whole loop.

## Check Wander from the command line

`skywalker check` compiles a file exactly as the engine would, including `use`d modules from a project:

```bash
skywalker check scripts/guard.wander --project ~/Games/MyGame
skywalker check scripts/guard.wander --format          # print the canonical formatting
skywalker check scripts/guard.wander --disassemble     # print the bytecode
```

The exit code is 0 when the file compiles, so the command fits a pre-commit hook or CI. A failure prints the position,
the message, the stable code and a hint:

```text
guard.wander:1:1: error: unknown trigger 'foobar' [unknown_trigger]
  hint: triggers: start, tick, event "name", key "name", click (and enter/exit in states)
```

## Pitfalls

- Events sent with `emit` arrive on the **next** tick, and `destroy` takes effect at the end of the tick. Tests wait
  `frames 2` before checking an event's effect.
- A handler that raises five runtime errors disables its script until play restarts. Read `logs` when an entity stops
  reacting.
- `wait` is allowed in handlers and tests, not inside `fn`.
- New keywords cannot be used as names: `fn`, `return`, `for`, `in`, `while`, `wait`, `until`, `state`, `go`, `test`,
  `expect`, `use`, `with`, `param`, `other`, `step` and others. The compiler reports `reserved_word` with a suggestion.

!!! agent "For agents"

    Read the language once, then loop on check, attach and test until every rule is covered:

    ```tool
    wander_reference {}                                      # syntax, idioms and every builtin
    wander_check {"source": "on tick\n  move self by (0, 0, -2 * dt)\nend\n"}   # compile without attaching
    behavior_set {"entity": "Mover", "name": "Forward", "intent": "Walk forward at 2 m/s.", "source": "on tick\n  move self by (0, 0, -2 * dt)\nend\n"}
    wander_test {"entity": "Mover", "name": "Forward"}       # run the test blocks in a sandbox
    behavior_spec {"entity": "Mover", "name": "Forward"}     # coverage of the spec rules
    logs {"limit": 20}                                       # runtime errors after a play session
    ```

## In this section

| Page | What it covers |
|---|---|
| [Language](language.md) | Declarations, triggers, statements, values and types, runtime semantics, `on frame`, idioms |
| [Specs and tests](testing.md) | Spec rules, `test` blocks, `wander_test` modes, coverage, traces and step loops, a worked example |
| [Graphs and native code](native.md) | The node graph, the bytecode VM, AOT compilation, native C++ modules, adding builtins |

## Reference

- Tools: [`wander_reference`](../../reference/tools/wander.md#wander_reference),
  [`wander_check`](../../reference/tools/wander.md#wander_check),
  [`behavior_set`](../../reference/tools/wander.md#behavior_set),
  [`wander_test`](../../reference/tools/wander.md#wander_test),
  [`behavior_spec`](../../reference/tools/wander.md#behavior_spec),
  [`wander_inspect`](../../reference/tools/wander.md#wander_inspect)
- [Wander builtins](../../reference/wander.md), generated from the builtin registry
- CLI: [`skywalker check`](../../reference/cli.md#check)
- Design document: [docs/WANDER.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/WANDER.md)
