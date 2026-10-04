# Graphs and native code

A Wander behavior has more than one form. You can edit it as text or as a node graph, and the engine can run it on its
bytecode virtual machine or as ahead-of-time compiled native code. When a game needs more than Wander offers, a project
can add C++ modules whose functions behave like built-in Wander functions. This page covers all of these layers and when
to use each.

| Layer | What it is | Use it for |
|---|---|---|
| Node graph | A visual form of the same behavior, converted losslessly to and from code | Editing logic visually; reviewing control flow |
| Bytecode VM | The default runtime: a typed compiler and a register VM | Everything, by default |
| AOT behaviors | Wander compiled to C++ and loaded as a native library | Hot arithmetic loops in Wander |
| Native C++ modules | C++ files in the project that register builtins and per-tick systems | Heavy inner loops, custom simulation, third-party C++ libraries |
| Engine builtins | C++ functions registered by an engine subsystem | Extending the engine itself |

## The node graph

`behavior_graph` turns a behavior (or raw `source`) into a node graph, and `behavior_from_graph` turns a graph back into
Wander code. The round trip is lossless: every example script in the repository is tested to go code → graph → code
and compile to the same bytecode. Graph and code are two views of one behavior, never two copies.

### The graph model

| Element | Meaning |
|---|---|
| Body | Each handler, function, test and state handler is a body with its own `entry` node. |
| Exec nodes | Statements (`rotate`, `if`, `emit`, `wait`, ...), chained by exec wires. Every statement has an `in` pin and a `next` output; `if` adds `then`, `elif n` and `else` outputs; loops add a `body` output. |
| Data nodes | Expressions: operators, calls, methods, member access, lists, maps, vectors, text. Their `value` output is wired into a statement's data input. |
| Pins | Each input and output has a `name` and a `kind` (`exec` or `data`). A data input that is not wired holds an inline `value`: a literal or a plain name. Any pin accepts any Wander expression. |
| Links | `{from, out, to, in}`: from a node's output pin to another node's input pin. |
| Layout | `x`, `y` per node. Positions are saved with the behavior, so the editor reopens the same layout. |

Comments on statements are kept through the round trip. `palette: true` adds every node type with its pins, which is
what an agent needs to create new nodes.

This behavior:

```wander
param spin = 90 in 0..360 "degrees per second"
on tick
  rotate self by (0, spin * dt, 0)
  if self.rotation.y > 180 then
    log "half turn"
  end
end
```

becomes this graph (`behavior_graph {"source": ...}`), shortened here to the handler body:

```json
{"format": "wander-graph", "version": 1,
 "behaviors": [{"name": "Main", "implicit": true,
   "vars": [{"name": "spin", "value": "90", "param": true, "min": "0", "max": "360", "doc": "degrees per second"}],
   "handlers": [{"id": "b0.h0", "kind": "handler", "trigger": "tick", "title": "on tick", "entry": "b0.h0/entry",
     "nodes": [
       {"id": "b0.h0/entry", "type": "entry", "title": "on tick", "inputs": [], "outputs": [{"name": "then", "kind": "exec"}], "x": 0, "y": 0},
       {"id": "b0.h0/s0:degrees.y", "type": "op", "props": {"op": "*"},
        "inputs": [{"name": "a", "kind": "data", "value": "spin"}, {"name": "b", "kind": "data", "value": "dt"}],
        "outputs": [{"name": "value", "kind": "data"}], "x": 10, "y": 236},
       {"id": "b0.h0/s0:degrees", "type": "vector", "props": {},
        "inputs": [{"name": "x", "kind": "data", "value": "0"}, {"name": "y", "kind": "data"}, {"name": "z", "kind": "data", "value": "0"}],
        "outputs": [{"name": "value", "kind": "data"}], "x": 70, "y": 118},
       {"id": "b0.h0/s0", "type": "rotate", "props": {},
        "inputs": [{"name": "in", "kind": "exec"}, {"name": "entity", "kind": "data", "value": "self"}, {"name": "degrees", "kind": "data"}],
        "outputs": [{"name": "next", "kind": "exec"}], "x": 300, "y": 0},
       {"id": "b0.h0/s1:if", "type": "op", "props": {"op": ">"},
        "inputs": [{"name": "a", "kind": "data", "value": "self.rotation.y"}, {"name": "b", "kind": "data", "value": "180"}],
        "outputs": [{"name": "value", "kind": "data"}], "x": 370, "y": 96},
       {"id": "b0.h0/s1", "type": "if", "props": {"branches": 1, "else": false},
        "inputs": [{"name": "in", "kind": "exec"}, {"name": "if", "kind": "data"}],
        "outputs": [{"name": "then", "kind": "exec"}, {"name": "next", "kind": "exec"}], "x": 600, "y": 0},
       {"id": "b0.h0/s1.then/s0", "type": "log", "props": {},
        "inputs": [{"name": "in", "kind": "exec"}, {"name": "value", "kind": "data", "value": "\"half turn\""}],
        "outputs": [{"name": "next", "kind": "exec"}], "x": 640, "y": 192}],
     "links": [
       {"from": "b0.h0/s0:degrees.y", "out": "value", "to": "b0.h0/s0:degrees", "in": "y"},
       {"from": "b0.h0/s0:degrees", "out": "value", "to": "b0.h0/s0", "in": "degrees"},
       {"from": "b0.h0/entry", "out": "then", "to": "b0.h0/s0", "in": "in"},
       {"from": "b0.h0/s1:if", "out": "value", "to": "b0.h0/s1", "in": "if"},
       {"from": "b0.h0/s0", "out": "next", "to": "b0.h0/s1", "in": "in"},
       {"from": "b0.h0/s1", "out": "then", "to": "b0.h0/s1.then/s0", "in": "in"}]}],
   "states": [], "tests": []}]}
```

Literals and names such as `spin`, `dt` and `self.rotation.y` sit inline on pins; only the computed `spin * dt` and the
vector are separate data nodes.

### How to edit a behavior as a graph

=== "Tool call"

    ```tool
    behavior_graph {"entity": "Coin", "name": "Spin", "palette": true}
    behavior_from_graph {"entity": "Coin", "name": "Spin", "graph": {"format": "wander-graph", "version": 1, "behaviors": [{"name": "Main", "implicit": true, "vars": [], "handlers": [{"id": "b0.h0", "kind": "handler", "trigger": "tick", "title": "on tick", "entry": "b0.h0/entry", "nodes": [{"id": "b0.h0/entry", "type": "entry", "title": "on tick", "inputs": [], "outputs": [{"name": "then", "kind": "exec"}], "x": 0, "y": 0}, {"id": "b0.h0/s0", "type": "log", "props": {}, "inputs": [{"name": "in", "kind": "exec"}, {"name": "value", "kind": "data", "value": "\"tick\""}], "outputs": [{"name": "next", "kind": "exec"}], "x": 300, "y": 0}], "links": [{"from": "b0.h0/entry", "out": "then", "to": "b0.h0/s0", "in": "in"}]}], "states": [], "tests": []}]}}
    ```

    `behavior_from_graph` returns the generated source and its diagnostics. With `entity` and `name` it also compiles
    and saves the behavior (rejected on errors unless `allow_errors`) and stores the node positions.

=== "Editor"

    Each behavior card in the **Details** panel has a **Code | Graph** switch. The graph canvas pans and zooms; drag a
    node by its header to move it; edit pin values and node properties in place (any Wander expression is accepted);
    add nodes from the palette with **Add**; draw a wire from an output pin to an input pin; right-click an input and
    choose **Disconnect** to remove a wire. Open the canvas in a large window from its toolbar.

    Every edit regenerates the Wander code. If the graph is incomplete, the code is not saved and the reason is shown.
    Unsaved text edits must be saved or reverted before you switch to the graph.

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/wander-graph.webp · A behavior card switched to Graph: an on tick entry node wired to rotate and if nodes, a multiply data node feeding the rotate vector, the Add palette button and zoom controls</div>

## The bytecode VM

The compiler parses, resolves names, infers types and emits **register bytecode**. Each handler, function and test is
a *proto*: a flat array of 8-byte instructions over a fixed register file that holds locals and temporaries, with
constant operands encoded inline.

| Design point | Effect |
|---|---|
| Names resolve at compile time | Locals are registers, behavior vars are slots (`GETVAR`), component fields are pre-resolved references, property names are interned. |
| Fused compare-and-jump | Conditions compile to a single instruction and short-circuit jump chains. |
| In-place number arithmetic | Arithmetic on numbers runs without allocation. |
| 24-byte values | A tag plus a double, vector, color, entity id or object pointer. Strings, lists and maps are reference-counted with copy-on-write. |
| Shared programs | Identical sources compile once: a thousand entities with the same behavior share one program. |

Print the bytecode with `skywalker check --disassemble` (or `wander_check` with `disassemble: true`). For the behavior
above:

```bash
skywalker check spin.wander --disassemble
```

```text
proto 0 Main.var spin (params 0, regs 1)
     0  L1    LOADK     R0 K0(90)
     1  L1    RET       0 0 0
proto 1 Main.on tick (params 0, regs 7)
     0  L3    LOADSELF  0 0 0
     1  L3    LOADK     R2 K1(0)
     2  L3    GETVAR    5 0 0
     3  L3    LOADENV   6 0 0
     4  L3    MUL       R3 R5 R6
     5  L3    LOADK     R4 K1(0)
     6  L3    MAKEVEC   1 2 3
     7  L3    CALL      R0 __rotate/2
     8  L4    LOADSELF  2 0 0
     9  L4    GETMEMBER R1 R2 .rotation
    10  L4    GETMEMBER R0 R1 .y
    11  L4    JMPCMP    not R0 > K2(180) -> 14
    12  L5    LOADK     R0 K3("half turn")
    13  L5    CALL      R0 __log/1
    14  L2    STOP      0 0 0
ok: 1 behavior(s), 17 instructions
```

Each line shows the instruction index, the source line, the opcode and its operands. The `if` became one `JMPCMP`.

## AOT-compiled behaviors

`wander_compile_native` translates a behavior's bytecode to C++ against a small C ABI, compiles it with the system
`clang++ -O2 -shared -fPIC -std=c++20` into `<project>/.skywalker/cache/aot/`, loads it and attaches it to the runtime.

- **Identical semantics.** Arithmetic, comparisons, branches, loops, vector math and pure math builtins run inline;
  every other instruction calls back into the VM's own implementation. Native and interpreted runs produce the same
  results, errors and step budget: VM and native traces are tested to be byte-identical.
- **Selective.** Protos that would mostly call back into the VM stay in the VM.
- **Cached.** Libraries are keyed by program hash, so unchanged behaviors load instantly. An edited behavior runs in the
  VM until you compile it again.
- **Safe fallback.** Any failure leaves the behavior on the VM, with diagnostics.

```tool
wander_compile_native {"entity": "Swarm"}       # this entity's behaviors
wander_compile_native {}                        # every behavior in the scene
wander_compile_native {"auto": true}            # compile everything natively at every play start
native_list {}                                  # which behaviors run as native code
```

!!! note

    AOT libraries are not shipped in packaged games: the standalone player runs Wander on its bytecode VM. Native C++
    modules are shipped ([Shipping](../shipping.md)).

## Native C++ modules

A project can contain C++ in `native/*.cpp`, written by people or agents against `skywalker/native/sdk.h`, a stable C
ABI handed to the module when it loads. A module can:

- read and write entities and components (typed transform accessors, reflection by component and field name, or JSON);
- read and write vars, spawn and destroy entities, emit events;
- read input, time and seeded randomness;
- **register Wander builtins**, typed and documented like the core ones, callable from every behavior and listed by
  `wander_reference`;
- **register per-tick systems**, which run every fixed tick while playing, after the behaviors, in registration order.

### How to create and build a module

```tool
native_template {"name": "gameplay"}     # writes native/gameplay.cpp and native/module.json
native_build {}                           # compile with the system clang++ and load
native_list {}                            # compiler, last build, loaded builtins and systems
```

`native_template` writes this starter module. Every module exports `sky_module_init`, which receives the API table and
registers what the module provides:

```cpp
// Native module for Skywalker: C++ that Wander behaviors call like builtins.
// Build: native_build. Reloads when play starts. API: skywalker/native/sdk.h.
#include <cmath>

#include "skywalker/native/sdk.h"

namespace {

// wave(t, frequency) -> number: a builtin every behavior can call: wave(time, 2)
void wave(SkyCall* call, const SkyValue* args, int argc, SkyValue* result, void*) {
    double t = sky_arg_number(call, args, 0);
    double f = argc > 1 ? sky_arg_number(call, args, 1) : 1.0;
    *result = sky_number(std::sin(t * f * 6.283185307179586));
}

// A per-tick system: spins every entity tagged "spinner" (runs after behaviors).
void spinners(SkyWorld* w, double dt, void*) {
    SkyEntity ids[256];
    size_t n = sky_sdk_api->find_tagged(w, "spinner", ids, 256);
    for (size_t i = 0; i < n && i < 256; ++i) {
        float r[3];
        if (sky_sdk_api->get_rotation(w, ids[i], r) == 0) {
            r[1] = std::fmod(r[1] + static_cast<float>(90.0 * dt), 360.f);
            sky_sdk_api->set_rotation(w, ids[i], r);
        }
    }
}

}  // namespace

SKY_MODULE_EXPORT int sky_module_init(const SkyApi* api, SkyModule* module) {
    SKY_SDK_INIT(api);
    SkyBuiltinDesc d = {"wave", "Sine wave in -1..1 for time t and a frequency (Hz).", "math", "wave(time, 2)",
                        1, 2, "t: number, frequency?: number", "number", wave, nullptr};
    if (api->register_builtin(module, &d) != 0) return -1;
    return api->register_system(module, "spinners", spinners, nullptr);
}
```

After `native_build`, `native_list` shows the builtin with its signature
(`wave(t: number, frequency: number?) -> number`) and the `spinners` system, and any behavior can call it. This
fragment compiles only in a project whose module is built, so it is shown as text:

```text
on tick
  self.position.y = 1 + wave(time, 0.5) * 0.25
end
```

### The SDK at a glance

`SkyBuiltinDesc` fields, in order: `name` (snake_case), `doc` (one sentence for `wander_reference`), `category`,
`example`, `min_args`, `max_args` (`-1` = any number), `params` (for docs and type checks, for example
`"a: number, b: vec|entity, c?: string"`), `returns` (a type name), `fn`, `user`.

| Group | `SkyApi` members |
|---|---|
| Registration (only inside `sky_module_init`) | `register_builtin`, `register_system` |
| Logging and errors | `log`, `last_error` |
| Builtin calls | `call_world`, `call_self`, `call_fail` (aborts the calling handler after return) |
| Time, randomness, input | `time`, `dt`, `frame`, `random` (seeded, replays exactly), `key_held` |
| Entities | `find`, `exists`, `api->entity_count` and `api->entity_at` (scene order), `find_tagged`, `has_tag`, `name`, `spawn`, `destroy` (deferred to the end of the tick) |
| Transforms (local to the parent) | `get_position`, `set_position`, `get_rotation`, `set_rotation` (Euler degrees), `get_scale`, `set_scale`, `world_position` |
| Components (reflection, same names as Wander) | `has_component`, `get_field`, `set_field`, `get_component_json`, `patch_component_json` |
| Vars | `get_var`, `set_var` |
| Events | `emit` (delivered next tick; target 0 broadcasts) |
| Values | `string_new`, `string_chars`, `list_new`, `list_length`, `list_get`, `list_push`, `map_new`, `map_get`, `map_set`, `copy`, `release` |

Helpers in the header: `SKY_SDK_INIT(api)` checks the SDK version, `sky_arg_number` and `sky_arg_entity` read typed
arguments (and fail the call on a wrong type), `sky_number`, `sky_bool`, `sky_vec` and `sky_none` build values. A
module may also export `sky_module_shutdown`, called before the library unloads. Values that hold strings, lists or
maps are reference-counted: never copy them with plain assignment; use `copy` and `release`.

### module.json

`native/module.json` configures the build:

```json
{
  "flags": ["-O3"],
  "include_dirs": ["third_party/include"],
  "lib_dirs": [],
  "libs": [],
  "frameworks": ["Accelerate"],
  "pkg_config": []
}
```

`pkg_config` names packages resolved with `pkg-config`, so a module can use any C++ library installed on the machine,
for example a Homebrew package. `native_build` compiles everything into
`<project>/.skywalker/cache/native/<name>_<hash>.dylib` and loads it. Compiler errors come back as
`{file, line, column, message}` relative to the project. Unchanged sources load the cached build instantly.

Modules hot-reload when play starts: if the sources changed, the module is rebuilt first, and a failing build keeps
the previous module loaded.

### Security model

Native modules and AOT libraries are trusted local code: they run inside the engine process with your user's
permissions, like any program built on the machine. The tools that build and load them (`native_build`,
`wander_compile_native`, `native_template`) are mutating tools in the `code` category, so MCP clients and the in-editor
crew ask before an agent uses them ([Permissions and approvals](../../agents/permissions.md)). Review agent-written C++
before you build it. Wander itself stays sandboxed: no file, network or clock access.

## Add a builtin to the engine in C++

Engine subsystems register their Wander functions in one registry (`skywalker/wander/Builtins.h`). Each `BuiltinDef`
carries the name, typed parameters, return type, category, documentation, example and implementation; the compiler
checks calls against it, `wander_reference` and the [builtins reference](../../reference/wander.md) are generated from
it, and the VM and AOT code dispatch through it.

```cpp
// engine/src/<subsystem>/<Name>Builtins.cpp
#include <cmath>

#include "skywalker/wander/Builtins.h"

namespace sky {

void registerSurveyBuiltins(wander::BuiltinRegistry& reg) {
    wander::BuiltinDef d;
    d.name = "distance_xz";
    d.params = {{"a", wander::kTPoint}, {"b", wander::kTPoint}};  // a vector or an entity's world position
    d.returns = wander::kTNumber;
    d.category = "vector";
    d.doc = "Horizontal distance between two points, ignoring height.";
    d.example = "if distance_xz(self, find(\"Flag\")) < 2 then log \"at the flag\" end";
    d.owner = "engine";
    d.fn = [](wander::CallContext& c) -> wander::Value {
        Vec3 a = c.point(0), b = c.point(1);  // typed accessors fail the call with a precise error
        float dx = a.x - b.x, dz = a.z - b.z;
        return wander::Value::number(std::sqrt(dx * dx + dz * dz));
    };
    reg.add(std::move(d));
}

}  // namespace sky
```

Then add one line, `registerSurveyBuiltins(reg);`, to `sky::registerEngineBuiltins` in
`engine/src/engine/EngineBuiltins.cpp`.

| `BuiltinDef` field | Meaning |
|---|---|
| `name`, `category`, `doc`, `example` | What `wander_reference` and the generated reference show |
| `receiver` | For methods (`list.push(x)`): the receiver types; 0 for a free function |
| `params` | `{name, type, optional}` per parameter; types are `kTNumber`, `kTVec`, `kTPoint` (vector or entity), `kTEntity`, `kTString`, `kTList`, `kTMap`, unions with `|` |
| `variadic` | Accepts any number of trailing arguments of the last parameter's type |
| `returns` | The return type set |
| `pure` | No side effects and deterministic in its arguments, so calls can be constant-folded |
| `mutates` | A method that changes its receiver, written back to the variable or property |
| `fn` | The implementation: `Value (*)(CallContext&)` |

Rules for builtins:

- Reach engine state through `c.service<Engine>()`. It is null in sandboxes and bare runtimes, so degrade gracefully or
  `c.fail(...)` with a clear message.
- Stay deterministic: use `c.rng()` for randomness and never read wall-clock time.
- Charge heavy work with `c.charge(units)` so it counts toward the step budget.
- New trigger words register with `reg.addTrigger({name, doc, payload, category})` and are delivered with
  `Runtime::emit(name, target, payload, other)`; scripts then write `on <name>`.

## Performance

`tools/wander_bench` times representative workloads at fixed 1/60 s ticks, measured on an M1 Pro with a Release build;
numbers are the best of four runs on a loaded machine. The "Previous interpreter" column is the tree-walking
interpreter Wander used before the bytecode VM; its scenarios use only the syntax that interpreter understood.

| Scenario | Previous interpreter | VM | Speedup | AOT | Speedup |
|---|---|---|---|---|---|
| `w1_compute`: arithmetic loop, 500 iterations × 100 entities | 12.24 ms | 0.93 ms | 13.2× | 0.37 ms | 33× |
| `w1_gameplay`: vars, movement, branches, timers × 5000 entities | 6.49 ms | 1.20 ms | 5.4× | 1.22 ms | 5.3× |
| `w1_vectors`: vector math loop, 100 iterations × 100 entities | 2.97 ms | 0.50 ms | 5.9× | 0.41 ms | 7.2× |
| `w1_builtins`: math builtins, 300 iterations × 100 entities | 27.20 ms | 1.99 ms | 13.7× | 0.93 ms | 29× |
| `overhead`: an empty `on tick` × 5000 entities | | 0.24 ms | | | |
| `w2_functions`: function calls, lists, for loops × 100 entities | | 1.44 ms | | 1.32 ms | |

`w1_gameplay` is mostly property access and engine calls, so the AOT heuristic leaves it in the VM. Run
`wander_bench` (VM) and `wander_bench --native` (AOT) to reproduce.

What this means in practice: thousands of entities with ordinary gameplay behaviors cost about a millisecond per tick
in the VM. Reach for AOT when a behavior runs long arithmetic loops, and for a native module when the work is an
algorithm (flocking, procedural generation, custom physics) or needs a C++ library.

## Pitfalls

- An edited behavior falls back to the VM until `wander_compile_native` runs again (or `auto: true` is set).
- A native module that links a non-system library must have that library available wherever the game runs; the
  packager warns when a module links something players may not have.
- Builtins registered by a native module exist only while the module is loaded: `skywalker check` on a file that calls
  them fails outside a project whose module is built.
- In the graph editor, an incomplete graph (a statement with an unwired required input) is not saved.

!!! agent "For agents"

    Choose the lightest layer that solves the problem, and measure before you go native:

    ```tool
    behavior_graph {"entity": "Guard", "name": "Guard"}       # show the logic visually to a human
    wander_check {"source": "on tick\n  let s = 0\n  for i in 0..1000\n    s += i * dt\n  end\nend\n", "disassemble": true}
    wander_compile_native {"entity": "Swarm"}                 # hot arithmetic loops: same results, faster
    native_template {"name": "flocking"}                      # algorithms or C++ libraries: write C++
    native_build {}                                           # structured compiler diagnostics on failure
    native_list {}                                            # confirm builtins and systems are loaded
    ```

    Native code runs with the user's permissions. Explain what the C++ does before asking to build it.

## Reference

- Tools: [`behavior_graph`](../../reference/tools/wander.md#behavior_graph),
  [`behavior_from_graph`](../../reference/tools/wander.md#behavior_from_graph),
  [`wander_check`](../../reference/tools/wander.md#wander_check),
  [`wander_compile_native`](../../reference/tools/code.md#wander_compile_native),
  [`native_build`](../../reference/tools/code.md#native_build),
  [`native_list`](../../reference/tools/code.md#native_list),
  [`native_template`](../../reference/tools/code.md#native_template)
- CLI: [`skywalker check`](../../reference/cli.md#check)
- [Wander builtins](../../reference/wander.md)
- Design document: [docs/WANDER.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/WANDER.md)
