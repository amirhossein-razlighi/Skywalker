# Wander 2 & ECPS

**ECPS (Entity · Component · Prompt System)** extends ECS with a third pillar: the *prompt*.

- **Entities** have **components** (data: transform, mesh, light, body, audio, …).
- They also have **behaviors**. Each behavior is an **intent**, a natural-language
  description written by a person or an agent, an optional **spec** (rules derived from the
  intent, each verified by tests), and its **Wander** code, a deterministic program the
  engine runs.

The intent is the source of truth for humans. The code is the source of truth for the
runtime. The spec and the code's `test` blocks connect the two: an agent derives rules from
the intent, writes code plus one test per rule, and proves the code meets the intent with
`wander_test` before anyone presses play.

- [Why Wander 2](#why-wander-2)
- [A tour](#a-tour)
- [Language reference](#language-reference)
- [Runtime semantics](#runtime-semantics)
- [Builtins and the registry](#builtins-and-the-registry)
- [From intent to code: spec and tests](#from-intent-to-code-spec-and-tests)
- [The graph view](#the-graph-view)
- [Execution: bytecode VM](#execution-bytecode-vm)
- [Native code: AOT behaviors and C++ modules](#native-code-aot-behaviors-and-c-modules)
- [Tools](#tools)
- [Performance](#performance)
- [Migrating from Wander 1](#migrating-from-wander-1)

## Why Wander 2

Wander 1 proved the idea (agents wrote correct behaviors on the first try) but it was a
small tree-walking interpreter with no functions, no collections and no loops beyond
`repeat`. Games need more, and agents need more structure to verify what they write.
Wander 2 is a redesign that keeps what worked and replaces what did not:

| Kept | Why |
|---|---|
| Keyword-led statements (`move self toward goal at 3`, `emit "x" to e`) | They read like the intent; LLMs write them reliably |
| Guaranteed termination | A buggy (or adversarial) script can never hang the engine |
| Determinism | Fixed ticks, seeded randomness, ordered events: runs replay exactly |
| Precise diagnostics | Line, column, stable code, and a did-you-mean hint |
| Intent + code per behavior | The ECPS model |

| Changed | Why |
|---|---|
| Tree-walking interpreter → **typed compiler + register bytecode VM** | 5–13× faster (see [Performance](#performance)); one place for checks |
| `repeat` only → `for`/`while`/`break`/`continue` bounded by an **instruction budget** | Real algorithms, still guaranteed to terminate |
| Hard-coded functions → a **builtin registry** | Every subsystem (physics, audio, input, native modules) registers typed, documented builtins in one place; references are generated |
| No abstraction → `fn`, recursion, `const`, **modules** (`use`) | Shared code across behaviors |
| Numbers and vectors only → **lists and maps** with value semantics | Inventories, waypoints, stats, payloads |
| Hand-rolled timers → **coroutines** (`wait`) and **state machines** | The two most common game patterns, readable and deterministic |
| Untyped → **optional types with inference** (gradual) | Mistakes are caught before play, without annotation noise |
| Events without data → **payloads** and `other` | `emit "damage" with {amount: 5} to target` |
| Manual playtesting → **`test` blocks** run in a sandbox | Agents verify behavior against the intent |
| Text only → a **node-graph view** with a lossless round trip | Blueprint-style editing for people who prefer it |
| Interpreted only → **AOT to C++** and **native C++ modules** | Hot loops at native speed; any C++ library |

## A tour

```wander
behavior Guard
  intent "Patrol between two posts; chase the player when close; lose 1 hp per hit."
  param speed = 3 in 0..10 "walk speed (m/s)"   -- tunable: a slider in the editor
  var hp = 3                                    -- per-entity state, visible to tools
  var posts: list = []

  on start
    posts = [self.position, self.position + (6, 0, 0)]
  end

  state Patrol                                  -- the first state is the initial one
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

  on event "damage" with hit                     -- payload map; `other` is the sender
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

## Language reference

### Files and declarations

A script is one or more `behavior Name … end` blocks, or bare members (then the whole file
is one behavior named `Main`). Around them, a file may hold `use`, `const`, `fn` and `test`
declarations shared by its behaviors.

| Declaration | Meaning |
|---|---|
| `intent "…"` | The behavior's natural-language intent |
| `var hp = 3` / `var target: entity? = none` | Per-entity state. Read as `hp` or `self.hp`. Initializers run when the behavior starts and never overwrite a value the entity already has (set in the editor, by an agent, or by a prefab) |
| `param speed = 3 in 0..10 "doc"` | A var that is a *tunable*: constant default, optional range and description; shown as a slider in the editor and listed in the spec |
| `const MAX = 10` | A compile-time constant (literals, arithmetic, other consts) |
| `fn name(a, b: number) -> number … end` | A function. Behavior-level fns can use the behavior's vars. Recursion is allowed (depth limit 200) |
| `on <trigger> … end` | A handler (see below) |
| `state Name … end` | A state holding handlers (`on enter`, `on exit`, `on tick`, `on event …`) |
| `test "name" … end` | An in-language test (see [spec and tests](#from-intent-to-code-spec-and-tests)) |
| `use "scripts/combat"` (`as c`) | A module: a `.wander` file of `fn` and `const` declarations in the project. Call `combat.damage(…)`, read `combat.MAX_HP` |

### Triggers

| Trigger | Fires |
|---|---|
| `on start` | Once, the first tick the behavior runs (after var initializers) |
| `on tick` | Every fixed tick (1/60 s) |
| `on event "name"` (`with x`) | When the event is delivered; the payload is `data` (or `x`), the sender is `other` |
| `on key "space"` | When the key is pressed |
| `on action "jump"` | When an input action (input.json) is pressed |
| `on click` | When the entity is clicked |
| `on collide ("name or tag")`, `on trigger_enter (…)`, `on trigger_exit (…)` | Physics contacts; `other`, `contact_point`, `contact_normal`, `impact` |
| `on enter`, `on exit` | Inside a `state`: when the state is entered / left |
| `on pause`, `on resume` | The game was paused / resumed (`pause_game()`, `resume_game()`); reaches every behavior, also those the pause stops |
| `on frame` | Every *displayed* frame (display rate, e.g. 120 Hz), cosmetic only: see [on frame](#on-frame-cosmetic-display-rate-handlers) |

Subsystems can register further trigger words (`wander_reference` lists them).

### Statements

| Statement | Notes |
|---|---|
| `let x = v`, `let x: number = v`, `const y = v` | Block-scoped locals |
| `x = v`, `set x to v`, `x += v` (`-= *= /=`) | Locals, vars, properties (`self.position.x = 1`), elements (`items[0] = 3`, `stats.hp = 2`) |
| `if c then … elif c then … else … end` | `then` is optional |
| `while c … end` | Bounded by the instruction budget |
| `for x in list`, `for i, x in list`, `for k, v in map`, `for ch in "text"` | Iterates a snapshot: changing the collection inside the loop does not affect the loop |
| `for i in 0..10` (10 excluded), `for i in 1..=10`, `for i in 10..0 step -2` | Numeric ranges |
| `repeat n times … end`, `break`, `continue` | |
| `return v`, `stop` | `stop` (or bare `return`) leaves a handler |
| `every 2 seconds … end`, `after 1 seconds … end` | Timers that advance when the statement runs |
| `wait 1.5`, `wait frames 3`, `wait until hp <= 0` | Suspend the handler (handlers and tests only, not inside `fn`) |
| `go to State` | Switch the behavior's state machine |
| `move e by v`, `move e toward p at speed`, `rotate e by (0, 90 * dt, 0)`, `look e at p` | Transforms (meters, degrees; entities count as their world position) |
| `emit "evt"` (`with payload`) (`to entity`) | Delivered next tick, in emission order |
| `destroy e`, `log value` | `destroy` is deferred to the end of the tick |
| `expect c ("why")`, `press "k"`, `hold "k"`, `release "k"`, `click e` | Tests only |

### Values, operators and properties

- Values: numbers (double), `"strings"` with `{interpolation}` (`\{` for a brace),
  `true`/`false`, `none`, colors `#rgb #rrggbb #rrggbbaa`, vectors `(x, y, z)` (2 components
  give z = 0, 4 components make a color), lists `[1, 2]`, maps `{hp: 3, "max hp": 5}`,
  entities.
- Operators: `+ - * / %`, `< <= > >= == !=`, `and or not`, `x in list/map/string`;
  `"text" + anything` joins; lists `+` lists concatenate; vectors and colors do component math.
- Names: `self`, `other`, `dt`, `time`, `frame`, `pi`, `state`, `state_time`, `data` (event
  payload), `contact_point`, `contact_normal`, `impact`, `hit_point`, `hit_normal`, `hit_distance`.
- Entity properties: `position rotation scale color name id enabled tags parent state`,
  components `e.light.intensity`, vars `e.hp`. Vectors: `.x .y .z .length`; colors
  `.r .g .b .a`; lists and strings `.length`; maps `.key`.
- **Lists and maps are values**: assignment copies (copy-on-write, so it is cheap), so
  `let copy = items` never aliases. Mutate in place with methods: `self.items.push(x)`.
  This keeps every value serializable (vars are JSON) and makes cycles impossible.
- Comments: `-- …`, `// …`, `# …`.

### Types

Annotations are optional: `number bool string vec color entity list map any none`, unions
`number|string`, and `T?` for "or none". Without annotations types are inferred (`let x = 3`
is a number; `var target = none` is `any`). The compiler reports an error only when a
mismatch is certain — `"a" - 1`, `distance("a", self)`, assigning a string to a `number`
var, `v.w` on a vector — and stays silent when a value could be several things. This is
gradual typing: no annotation noise, and agents still get type errors before play.

## Runtime semantics

- Every fixed tick the runtime visits active entities in scene order, and for each script:
  starts it (var initializers, `on start`, the initial state's `on enter`), resumes waiting
  handlers whose wait elapsed, delivers last tick's events, then physics contacts, key
  presses, input actions and clicks, then runs `on tick` (behavior-level first, then the
  current state's).
- **Coroutines.** A handler that hits `wait` is suspended with its locals and resumes on a
  later tick (`wait 0.5` = 30 ticks at 60 Hz; `wait frames 3` = 3 ticks; `wait until c`
  re-checks once per tick). A waiting `on tick` handler does not start again until it
  finishes; other handlers start a new run per trigger (at most 64 waiting runs per
  script). Leaving a state cancels the waiting runs of that state's handlers. Stopping
  play clears everything.
- **State machines.** The first declared state is the initial one. `go to X` ends the
  handler, runs the old state's `on exit`, then the new state's `on enter`; `state` and
  `state_time` read the current one; `e.state` reads another entity's. More than 32
  transitions in one tick (states bouncing) is an error.
- **Vars** live in fast per-entity tables while playing and are mirrored into the entity's
  `vars` after every tick, so `entity_get` and the inspector always show current values;
  edits made between ticks (by tools) are picked up on the next tick.
- **Termination.** Each handler run has a budget of 1,000,000 steps: every loop iteration
  charges the loop's length and every call the callee's length (heavy builtins such as
  `sort` charge per element). Exceeding it aborts the run with an error at the loop's line.
- **Errors.** A runtime error (with line, and file for module code) aborts that handler run
  and is reported in `logs`; after 5 errors the script is disabled.
- **Determinism.** Randomness comes from the scene's seed (`random`, `chance`, `pick`,
  `shuffle`, `random_int`); there is no wall-clock access. Two runs from the same scene
  produce the same trace, in the VM and in native code.
- **Limits.** 256 `spawn`s per tick, 20,000 entities; `destroy` takes effect at the end of
  the tick; spawned entities start their behaviors next tick.
- **Pause and time scale.** `pause_game()` / `resume_game()` / `time_scale(x)` apply from the
  next tick. While the game is paused, behaviors of `pausable` entities (the default) are
  stopped: no `on tick`, no waits or timers advancing, no input; they still receive `on pause`
  and `on resume`, and events they have handlers for wait and arrive on resume. Entities with
  `process.mode` `always` or `when_paused` (and UI canvases, which run `always` by default)
  keep running — that is where pause menu logic goes. `dt` is scaled by the time scale (and
  is real for entities on `process.clock: real`); `time` is game time (it stops while paused);
  `unscaled_dt()` / `unscaled_time()` are real time. `process.priority` reorders behaviors
  (lower first). See docs/ARCHITECTURE.md "Game pause, process modes and time scale".
- Replacing a behavior while playing restarts its instance. `use`d modules reload when
  play starts or when their files change.

## on frame: cosmetic display-rate handlers

`on tick` runs at the fixed 60 Hz. On a 120 Hz display the engine already shows motion between
ticks (render interpolation), but some touches are best computed per displayed frame: camera
shake, bobbing, flicker, UI tweens. `on frame` handlers run once per displayed frame while the
game plays, after interpolation, with `dt` = real seconds since the last frame (scaled by the
time scale) and `time` = the displayed game time.

They are **cosmetic**: everything they write is undone after the frame, so the simulation (and
every replay, test and trace) is identical however many frames are shown. They may:

- set `position`, `rotation`, `scale`, `color` and fields of `transform`, `mesh`, `light`,
  `camera`, `sprite`, `text`, `ui`, `light2d` (on entities that have them);
- read anything (vars, components, other entities) and call pure functions and read-only
  queries (`find`, `distance`, `key`, `axis`, `raycast`, `anim_state`, `noise`, `log`...).

They may not change vars, `wait`, use `every`/`after`, `go to`, `move`/`rotate`/`look`,
`emit`, `spawn`, `destroy` or call `random` (it advances the seeded generator; use `noise`).
The compiler reports these as `frame_not_cosmetic` with a hint; functions they call are checked
at run time (the first violation stops that script's frame handlers until the next play).

```text
behavior CameraShake
  var trauma = 0                       -- raised by `on event "hit"` in on tick code
  on event "hit"
    trauma = min(1, trauma + 0.5)
  end
  on tick
    trauma = max(0, trauma - dt * 1.5)
  end
  on frame
    let k = trauma * trauma * 0.3
    self.position = self.position + (noise(time * 40) - 0.5, noise(time * 40 + 9) - 0.5, 0) * k
  end
end
```

## Builtins and the registry

Every function scripts can call lives in one registry (`wander/Builtins.h`): name, typed
parameters, return type, category, one-line doc, example and implementation. The compiler
checks calls against it (arity, types, did-you-mean), `wander_reference` and the editor are
generated from it, and the VM and native code dispatch through it. Categories today:
`math`, `vector`, `color`, `random`, `scene`, `input`, `text`, `list`, `map`, `effects`,
`audio`, `physics`, `character`, `navigation`, plus whatever native modules add. Call
`wander_reference` (or `wander_reference topic="physics"`) for the full, current list.

Methods are registered with a receiver type: `list.push(x)`, `map.get(k, default)`,
`"text".split(",")`. Methods that change their receiver write the result back to where it
came from, so `self.inventory.push(item)` updates the var.

Adding builtins from a subsystem:

```cpp
// engine/src/<subsystem>/<Name>Builtins.cpp
void registerWeatherBuiltins(wander::BuiltinRegistry& reg) {
    wander::BuiltinDef d;
    d.name = "wind_at";
    d.params = {{"point", wander::kTPoint}};
    d.returns = wander::kTVec;
    d.category = "weather";
    d.doc = "Wind velocity (m/s) at a point.";
    d.example = "push(self, wind_at(self) * 0.2)";
    d.fn = [](wander::CallContext& c) -> wander::Value {
        Engine* engine = c.service<Engine>();      // null in sandboxes: degrade gracefully
        Vec3 p = c.point(0);                        // typed accessors with precise errors
        return wander::Value::vec(engine ? engine->weather().windAt(p) : Vec3{});
    };
    reg.add(std::move(d));
}
```

and one line in `sky::registerEngineBuiltins` (`engine/src/engine/EngineBuiltins.cpp`).
New trigger words (`on contact`) register with `reg.addTrigger(...)` and are delivered with
`Runtime::emit(name, target, payload, other)`. Builtins must be deterministic: use
`c.rng()` and never read wall-clock time.

## From intent to code: spec and tests

The pipeline from natural language to verified code:

1. **Intent** — a person writes what the entity should do, in plain language.
2. **Spec** — an agent derives rules from the intent and stores them with the behavior
   (`behavior_set spec={summary, rules: [{text, tests: [...]}]}`). Each rule names the tests
   that verify it. Tunables come from `param` declarations (with ranges), triggers and
   states from the code.
3. **Code with tests** — the agent writes the behavior plus one `test` block per rule.
4. **Verify** — `wander_test` runs every test in a sandbox: a fresh world with a copy of the
   entity (or of the whole scene), ticked once so `on start` ran; `wait` advances simulated
   time, `emit`/`press`/`hold`/`click` inject input, `expect` records failures with the
   compared values (`expected hp == 2 — left side was 3, right side was 2`).
5. **Coverage** — `behavior_spec` shows rules without tests and tests without rules, so
   intent, spec and code stay in sync as any of them changes.

```wander
behavior Coin
  intent "Spins; when the player touches it, it adds 1 to the score and disappears."
  param spin = 90 in 0..360 "degrees per second"
  on tick
    rotate self by (0, spin * dt, 0)
  end
  on trigger_enter "player"
    emit "score" with {points: 1}
    destroy self
  end

  test "spins"
    let before = self.rotation.y
    wait 0.5
    expect self.rotation.y != before
  end
end
```

## The graph view

`behavior_graph` turns a behavior into a Blueprint-style node graph and
`behavior_from_graph` turns a graph back into code; the round trip is lossless (every
example script in the repository is tested: code → graph → code compiles to the same
bytecode).

- Each handler, fn, test and state handler is a **body** with an entry node.
- **Statements** are exec-flow nodes chained by exec wires: `if` has `then`/`elif n`/`else`
  outputs, loops a `body` output, every statement a `next` output.
- **Expressions** are data nodes (operators, calls, methods, member access, lists, maps,
  vectors, text) wired into statement inputs; literals and plain names sit inline on input
  pins (`value`), like pin defaults in Unreal. Any pin accepts any Wander expression.
- Comments on statements are kept. Node positions are saved with the behavior.

In the editor, each behavior card has a **Code | Graph** switch. The graph canvas pans and
zooms, nodes move by dragging their header, pin values and node properties are edited in
place, nodes come from the palette (Add), and wires are drawn from an output pin to an input
pin (right-click an input to disconnect). Every edit regenerates the Wander code; if the
graph is incomplete the code is not saved and the reason is shown.

## Execution: bytecode VM

The compiler (`wander/Compiler.h`) parses, resolves names, infers types and emits a
**register bytecode** (`wander/Bytecode.h`): each handler, fn and test is a proto with a flat
array of 8-byte instructions over a fixed register file (locals and temporaries), with
constant operands encoded inline. Notable design points:

- Names resolve at compile time: locals are registers, behavior vars are slots
  (`GETVAR`), components and fields are pre-resolved references, property names are
  interned.
- Conditions compile to fused compare-and-jump instructions and short-circuit jump chains;
  arithmetic on numbers runs in place without allocation.
- Values are 24 bytes (a tag plus a double, vector, color, entity id or object pointer);
  strings, lists and maps are reference-counted with copy-on-write.
- Identical sources share one compiled program (thousands of entities with the same
  behavior compile once).
- `skywalker check file.wander --disassemble` (or `wander_check disassemble=true`) prints
  the bytecode.

## Native code: AOT behaviors and C++ modules

### AOT-compiled behaviors

`wander_compile_native` translates a program's bytecode to C++ against a small C ABI
(`skywalker/native/aot.h`, embedded in the generated file), compiles it with the system
`clang++ -O2 -shared -fPIC -std=c++20` into `<project>/.skywalker/cache/aot/`, `dlopen`s it
and attaches it to the runtime. Arithmetic, comparisons, branches, loops, vector math and
pure math builtins run inline; every other instruction calls back into the VM's own
implementation (`exec` / `exec_range`), so native and interpreted runs are identical by
construction — the same results, errors and step budget (tested: VM and native traces are
byte-identical). Protos that would mostly call back stay in the VM. Libraries are cached by
program hash, so unchanged behaviors load instantly; an edited behavior runs in the VM
until compiled again (`auto=true` compiles everything at every play start). Any failure
leaves the behavior on the VM with diagnostics.

### Native C++ modules

A project can contain C++ in `native/*.cpp`, written by people or agents against
`skywalker/native/sdk.h` — a stable C ABI handed to the module at load time
(`sky_module_init(const SkyApi*, SkyModule*)`). A module can read and write entities and
components (typed transform accessors and reflection by component/field name, or JSON),
read and write vars, spawn and destroy, emit events, read input, time and seeded
randomness, and **register Wander builtins** (typed and documented like core ones) and
**per-tick systems** (run after behaviors, in registration order).

```cpp
#include "skywalker/native/sdk.h"

static void fib(SkyCall* call, const SkyValue* args, int, SkyValue* result, void*) {
    double n = sky_arg_number(call, args, 0), a = 0, b = 1;
    for (int i = 0; i < (int)n; ++i) { double t = a + b; a = b; b = t; }
    *result = sky_number(a);
}

SKY_MODULE_EXPORT int sky_module_init(const SkyApi* api, SkyModule* module) {
    SKY_SDK_INIT(api);
    SkyBuiltinDesc d = {"fib", "n-th Fibonacci number", "math", "fib(20)", 1, 1, "n: number", "number", fib, nullptr};
    return api->register_builtin(module, &d);
}
```

`native/module.json` adds `flags`, `include_dirs`, `lib_dirs`, `libs`, `frameworks` and
`pkg_config` packages, so a module can use any C++ library (for example a Homebrew package
through pkg-config). `native_build` compiles everything into
`<project>/.skywalker/cache/native/<name>_<hash>.dylib` and loads it; compiler errors come
back as `{file, line, column, message}` relative to the project. Modules hot-reload when
play starts (rebuilt first if their sources changed; a failing build keeps the previous
module). `native_template` writes a starter module, `native_list` shows what is loaded.

**Security model.** Native modules and AOT libraries are trusted local code: they run
inside the engine process with the user's permissions, just like any program built on the
machine. The tools that build and load them are mutating tools in the `code` category, so
MCP clients and the in-editor crew ask before an agent uses them; review agent-written C++
before building it. Wander itself stays sandboxed (no file, network or clock access).

## Tools

| Tool | What it does |
|---|---|
| `wander_reference` | The guide plus every builtin (generated); `topic=` for details |
| `wander_check` | Compile without attaching: diagnostics, summary; `format`, `disassemble` |
| `behavior_set` | Save intent, spec and code (rejects code with errors) |
| `behavior_remove` | Remove a behavior |
| `behavior_spec` | Intent, spec, what the code declares, test coverage of the rules |
| `wander_test` | Run `test` blocks in a sandbox |
| `behavior_graph` / `behavior_from_graph` | Code ⇄ node graph (with saved layout) |
| `wander_inspect` | Running states, waiting handlers and vars of an entity |
| `wander_compile_native` | AOT-compile behaviors to native code |
| `native_build` / `native_list` / `native_template` | Native C++ modules |
| `sim_input` | Inject input and events (`event` + `data` payload) |

CLI: `skywalker check file.wander [--project DIR] [--format] [--disassemble]`.

## Performance

`tools/wander_bench` times representative workloads (fixed 1/60 s ticks; M1 Pro, Release).
The `w1_*` scenarios only use syntax Wander 1 understood, so they compare directly with the
old tree-walking interpreter:

| Scenario | Wander 1 | Wander 2 VM | Speedup | Wander 2 AOT | Speedup |
|---|---|---|---|---|---|
| `w1_compute`: arithmetic loop, 500 iterations × 100 entities | 12.24 ms | 0.93 ms | 13.2× | 0.37 ms | 33× |
| `w1_gameplay`: vars, movement, branches, timers × 5000 entities | 6.49 ms | 1.20 ms | 5.4× | 1.22 ms¹ | 5.3× |
| `w1_vectors`: vector math loop, 100 iterations × 100 entities | 2.97 ms | 0.50 ms | 5.9× | 0.41 ms | 7.2× |
| `w1_builtins`: math builtins, 300 iterations × 100 entities | 27.20 ms | 1.99 ms | 13.7× | 0.93 ms | 29× |
| `overhead`: an empty `on tick` × 5000 entities | — | 0.24 ms | | | |
| `w2_functions`: fn calls, lists, for loops × 100 entities | — | 1.44 ms | | 1.32 ms | |

¹ Mostly property access and engine calls: the AOT heuristic leaves it in the VM.

Run `wander_bench` (VM) and `wander_bench --native` (AOT) to reproduce; numbers are the
best of four runs on a loaded machine.

## Migrating from Wander 1

Wander 2 accepts almost all Wander 1 code unchanged (every example script in the
repository compiles and is tested). The differences:

- New keywords cannot be used as names: `fn return for in while do break continue wait until
  state go goto const test expect use with param other step`… (the compiler says
  `reserved_word` with a suggestion). One example script renamed a `wait` var.
- `repeat n times` has no 1000 cap any more; the step budget bounds all work.
- `and`/`or` still produce booleans; `0`, `""`, `none`, empty lists and maps are false.
- Setting `color` on an entity without a mesh now sets its light's color if it has one.
- Builtins are registered, not hard-coded: subsystem functions (physics, audio, input) are
  the same names as before, now documented in `wander_reference`.
