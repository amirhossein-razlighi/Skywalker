# Custom tools: agents define their own tools

The engine ships about 180 tools. When an agent needs something none of them does (a query
specific to this game, a repeated multi-step edit, a bridge to its own process), it defines a
**custom tool**. A custom tool is an ordinary tool from then on: it is listed in `tools/list`
(MCP clients get `notifications/tools/list_changed`), offered to the in-editor crew, callable
from `batch`, and every call is validated against its schema, attributed and logged.

What keeps this safe is the same set of rules for every custom tool:

- **Least privilege.** A tool declares its capabilities as an allowlist: reading the scene,
  editing it, the tools it may call, the project folders it may read or write, the network.
  A call runs with the caller's permissions intersected with those capabilities.
- **Limits.** Instruction budget, wall time, output size, number of nested calls, and nesting
  depth (at most 8 custom tools deep).
- **Undo.** Edits go through the engine's transactions: a tool call is one undo step, attributed
  to whoever called the tool, and a failing call leaves nothing behind.
- **Approval.** A per-project policy decides which tools a human must approve before they run.
  An approval is bound to the definition's hash, so changing the tool needs approval again.
- **Audit.** Definitions, approvals, registrations and every call (with its nested calls) are
  appended to `.skywalker/logs/custom_tools.jsonl` and shown in the activity feed.

## Kinds

| Kind | Implementation | Use it for |
|---|---|---|
| `wander` | Wander code with `fn run(args) ... end`, run in-process by the VM | Queries and computations over the scene; edits that need logic |
| `composite` | A pipeline of tool calls with templated arguments, loops and a result mapping | Repeating a known sequence of tool calls; the safest kind |
| `external` | Served by a connected client (the Python agent layer, another process) | Anything that needs a library, a model or the network |

Names are namespaced so they never shadow engine tools: project tools are `user_<name>`;
external tools are `<namespace>_<name>` (`py_` for the Python layer). Model APIs reject `.`
and `:` in tool names, so `user.enemies_near` is accepted and stored as `user_enemies_near`.

## Lifecycle tools

| Tool | What it does |
|---|---|
| `tool_define` | Create or update a tool: validates the schema and capabilities, compiles the code, runs the tests as dry runs, saves and registers it |
| `tool_test` | Run a tool's tests, or one call (`args`), or an unsaved `definition`, as a dry run |
| `tool_list_custom` | Every custom and external tool with its status, plus the policy; `reload` rescans `tools/`, `include_library` lists the user library |
| `tool_inspect` | Definition, code, approval, stats, recent calls and errors |
| `tool_remove` | Unregister and delete the files |
| `tool_enable` | Disable a tool without losing it, or enable it again |
| `tool_approve` | A human approves (or rejects) the current definition |
| `tool_promote` | Copy a tool into the user library (`~/.skywalker/tools`) for other projects |
| `tool_policy` | Read the policy, or make it stricter (only a human may loosen it) |

Statuses: `active` (callable), `pending_approval`, `rejected`, `disabled`, `offline` (an
external tool whose client is not connected), `invalid` (the file does not parse or validate;
the error is in `reason`), `policy_off`.

## The definition

Saved as `tools/<name>.tool.json` (Wander code goes to `tools/<name>.wander` next to it). The
files are plain JSON and Wander, meant to be committed and reviewed like code.

```json
{
  "format": "skywalker.tool/1",
  "name": "user_enemies_near",
  "title": "Enemies near",
  "description": "Enemies (tag enemy) within a radius of a point, with their hp. Use it before planning an encounter.",
  "category": "scene",
  "kind": "wander",
  "input_schema": {"type": "object", "properties": {"radius": {"type": "number", "default": 10}}, "additionalProperties": false},
  "output_schema": {"type": "object"},
  "capabilities": {"read_scene": true, "mutate": false, "calls": [], "network": false},
  "limits": {"instructions": 1000000, "timeout_ms": 5000, "max_output_bytes": 65536, "max_calls": 100},
  "source": "tools/enemies_near.wander",
  "tests": [{"name": "finds the goblin", "args": {"radius": 5}, "expect": {"result": {"count": {"$gte": 1}}}}],
  "enabled": true,
  "version": 3,
  "author": "agent:mira",
  "provenance": {"created_by": "agent:mira", "created_at": "2026-10-04T12:00:00Z", "updated_by": "mcp:claude-code"}
}
```

| Field | Notes |
|---|---|
| `name` | Lowercase letters, digits, underscores; `user_` is added if missing |
| `description` | For language models: what it does, when to use it, key arguments, an example (at least 20 characters) |
| `category` | The permission category the crew's per-category permissions use (`scene`, `world`, `custom`...) |
| `input_schema` | A JSON schema object. Closed by default (`additionalProperties: false`) so misspelled arguments get did-you-mean errors; `default`s are filled in before the call |
| `output_schema` | Optional; a result that does not match fails with `invalid_output` |
| `wander` / `source` | Inline code, or a project file holding it (`tool_define` always writes the file) |
| `steps`, `result` | Composite pipeline (below) |
| `tests` | `{name, args, setup, expect}` (below) |
| `version` | Bumped by `tool_define` whenever what the tool does changes |

### Capabilities

| Capability | Default | Meaning |
|---|---|---|
| `read_scene` | `true` | Query entities, components and vars. `false` runs the code against an empty scene (pure computation) |
| `mutate` | `false` | Edit the scene, directly or through tools that modify the project. Needs approval under the default policy |
| `calls` | `[]` | Tools it may call (`call_tool`, composite steps, an external tool's callbacks). Names or globs (`entity_*`). Composites derive it from their steps when empty. The `tool_*` lifecycle tools are never callable |
| `files.read` / `files.write` | `[]` | Project folders (`"data/"`) or globs (`"levels/*.json"`). No absolute paths, no `..`, symlinks may not lead outside the project. `tools/`, `game.json`, `.skywalker/`, `agents/` and `studio/` are never writable. Writing needs approval |
| `network` | `false` | External tools only: the tool reaches outside the project (MCP clients and the crew then ask before calling it) |

Checks happen twice: when the tool is defined (a `calls` entry that modifies the project
requires `mutate`, unknown tools get did-you-mean errors) and on every call.

### Limits

| Limit | Default | Max |
|---|---|---|
| `instructions` | 1 000 000 | 50 000 000 |
| `timeout_ms` | 5 000 (external: 30 000) | 120 000 |
| `max_output_bytes` | 65 536 | 1 048 576 |
| `max_calls` | 100 | 10 000 |

The instruction budget bounds computation (loops charge per iteration, as in gameplay
scripts). Wall time is checked around every nested call and after the code returns; a nested
custom tool keeps its caller's deadline when that is sooner.

## Wander tools

The code is a Wander file of `fn`, `const` and `use` declarations with a top-level
`fn run(args)`; `args` is a map of the arguments (schema defaults filled in), and what `run`
returns is the result (`{"result": ...}` in the tool's structured output, with `logs` and
`warnings` when there are any).

```wander
-- tools/enemies_near.wander
fn run(args)
  let out = []
  for e in find_all("enemy")
    if distance(e, (0, 0, 0)) <= args.radius then out.push({name: e.name, hp: e.hp}) end
  end
  return {count: out.length, enemies: out}
end
```

The whole Wander library is available (math, vectors, lists, maps, `find`, `find_all`,
`nearest`, entity properties and vars, component fields), plus these builtins, which only
exist inside tools:

| Builtin | Does |
|---|---|
| `call_tool(name, args?)` | Calls an allowlisted tool and returns its structured result; fails the tool on error |
| `try_tool(name, args?)` | The same, returning `{ok, result}` or `{ok: false, error, message, hint}` |
| `tool_warn(message)` | Adds a warning to the result |
| `tool_fail(message, hint?)` | Stops with error `tool_failed` (edits are rolled back) |
| `read_file(path)`, `list_files(folder)`, `file_exists(path)` | Project files the tool may read |
| `write_file(path, text)` | A project file the tool may write (skipped in dry runs) |
| `json_parse(text)`, `json_text(value, indent?)` | JSON in and out |
| `tool_actor()` | Who called the tool |

Values crossing the tool boundary: entities become their numeric ids (find them again with
`find("#12")`), vectors become `[x, y, z]` and `[x, y, z]` arrays become vectors, colors are
`"#rrggbb"`.

**Read-only tools** (no `mutate`) cannot assign entity properties, vars or component fields:
the assignment fails at once with `capability_denied`. If the code calls a builtin that could
edit the scene (`spawn`, `move`, `add_tag`...), the scene is also compared before and after the
call and any change is undone. **Mutating tools** run inside one engine transaction: direct
assignments are recorded as they happen, `spawn`/`destroy` and nested tool calls join the same
undo step, and an error rolls everything back. Randomness is seeded (`random()` repeats from
call to call); there is no wall clock.

Cost: a read-only tool that only reads costs what its code does (about 0.2 ms for a 1 250-enemy
scan in a 5 000-entity scene). A mutating tool pays a snapshot of each entity it edits; one that
calls move/rotate-style builtins snapshots every entity first (about 40 ms at 5 000 entities).

## Composite tools

```json
{
  "name": "tag_by_tag",
  "description": "Adds a tag to every entity that has another tag, as one undo step. Example: {\"from\": \"enemy\", \"add\": \"hostile\"}",
  "input_schema": {"type": "object", "properties": {"from": {"type": "string"}, "add": {"type": "string"}}, "required": ["from", "add"]},
  "capabilities": {"mutate": true},
  "steps": [
    {"id": "found", "tool": "scene_query", "args": {"tag": "{{args.from}}"}},
    {"tool": "entity_update", "for_each": "{{steps.found.matches}}", "as": "m",
     "args": {"entity": "{{m.id}}", "tags": ["{{args.from}}", "{{args.add}}"]}}
  ],
  "result": {"tagged": "{{steps.found.matches.length}}"}
}
```

| Step field | Meaning |
|---|---|
| `tool`, `args` | The call. Templates are resolved first |
| `id` | Name for later templates: `{{steps.<id>}}` is the step's structured result |
| `for_each` | A template giving an array (one call per element) or a count; results become an array |
| `as` | The loop variable (default `item`); `{{index}}` is the position |
| `when` | A template; the step is skipped when it is false, 0, empty or missing |
| `continue_on_error` | Keep going; failures are listed in `step_errors` |

Templates: `"{{expr}}"` as the whole string keeps the value's type (`"{{p}}"` passes an array);
inside text it is interpolated (`"Lamp {{index}}"`). Expressions are paths rooted at `args`,
`steps`, `prev` (the previous step), the loop variable, `index` and `actor`, with `.field`,
`[n]`, `.length` and a default after `??` (`{{args.count ?? 5}}`). A path that does not exist
is an error naming the missing field, with a did-you-mean.

A mutating composite is one undo step; when a step fails, everything before it is rolled back.

## Tests

`tests` run whenever the tool is defined (failing tests block the save) and with `tool_test`.
Each runs as a **dry run**: `setup` calls and the tool's own edits are rolled back, file writes
are skipped, and nothing is counted in the tool's stats. Tools that still need approval can be
tested.

```json
{"name": "finds the goblin",
 "setup": [{"tool": "entity_create", "args": {"name": "G", "tags": ["enemy"], "position": [1, 0, 0], "vars": {"hp": 1}}}],
 "args": {"radius": 5},
 "expect": {"result": {"count": {"$gte": 1}, "enemies": {"$len": {"$gte": 1}}}}}
```

`expect`: `ok` (default true unless `error` is given), `error` (the expected error code),
`result` (a subset match: objects match the keys you give, arrays element by element, numbers
within 1e-6), `contains` (text in the output), `max_ms`. Operators inside `result`: `$gte`,
`$lte`, `$gt`, `$lt`, `$len`, `$contains`, `$type`, `$exists`.

## Policy and approval

`game.json`:

```json
{"customTools": {"policy": "auto"}}
```

| Policy | Read-only tools | Tools that mutate, write files or reach the network |
|---|---|---|
| `off` | disabled | disabled |
| `ask` | need approval | need approval |
| `auto` (default) | run at once | need approval |
| `trust` | run at once | run at once |

Only humans approve: the actors `user` (the editor) and `cli` (`skywalker call ...` in a
terminal). Agents get `approval_requires_human` with instructions for the human:

```bash
skywalker call tool_list_custom --project my_game            # see what waits
skywalker call tool_inspect '{"name":"user_rebalance_pickups"}' --project my_game
skywalker call tool_approve '{"name":"user_rebalance_pickups"}' --project my_game
```

Approvals live in `tools/approvals.json` with the definition hash, who approved and when.
Agents may make the policy stricter (`tool_policy`), not looser, and `game_settings` refuses
to change `customTools` for agents.

**Permissions.** Calls run as the caller: a crew agent whose permissions turn a category off
cannot reach it through a tool, and a nested call that would need the human's OK is refused
unless the tool call itself was approved by the human. Edits are attributed to the caller.

**What this does not protect against.** The boundary is the engine's tool surface. An agent
that can also run shell commands or edit files directly (outside the engine) can change tool
files and `tools/approvals.json` like any other file; review those in version control.

## External tools

A connected client can host tools. The engine forwards each call to it and waits; while it
waits, the client may call back into the engine, and those callbacks run with the external
tool's declared capabilities and the original caller's identity. Two transports share one
registration path (the same validation, policy, approvals, limits, stats and audit):

- **MCP requests** on the client's own connection (stdio `skywalker mcp`, the editor's socket,
  `skywalker serve`): the engine sends `skywalker/tools/call` requests to the client. Works on
  every transport, including stdio, and from the engine's main thread.
- **Polling** with `tool_host_register` / `tool_host_poll` / `tool_host_reply` (tools named
  `py_<name>`; see [PYTHON_AGENTS](PYTHON_AGENTS.md)): for clients that cannot answer requests on
  their connection. Calls made on the engine's own thread fail fast with a hint.

### Protocol (MCP requests)

The server advertises it in `initialize`:
`capabilities.experimental["skywalker/externalTools"] = {version: 1, methods: [...], callMethod: "skywalker/tools/call"}`.

1. **Register** (client to engine). Definitions use MCP field names (`inputSchema`) or the file
   names (`input_schema`); `namespace` defaults to `ext`.

   ```json
   {"jsonrpc":"2.0","id":7,"method":"skywalker/tools/register","params":{
     "namespace":"py","persist":false,
     "tools":[{"name":"memory_recall","description":"Search the team's shared memory for notes about a topic",
               "inputSchema":{"type":"object","properties":{"query":{"type":"string"}},"required":["query"]},
               "capabilities":{"calls":["scene_query"],"mutate":false},
               "limits":{"timeout_ms":20000}}]}}
   ```

   Result: `{"tools":[{"name":"py_memory_recall","status":"active","hash":"..."}], "errors":[...], "policy":"auto", "call_method":"skywalker/tools/call"}`.
   Invalid definitions are reported per tool in `errors` (code, message, hint); the others are
   registered. A tool with `capabilities.mutate` waits for approval under the default policy.
   Every connected client gets `notifications/tools/list_changed`.

2. **Calls** (engine to client):

   ```json
   {"jsonrpc":"2.0","id":"sky-3","method":"skywalker/tools/call","params":{
     "name":"py_memory_recall","arguments":{"query":"lava"},"call_id":"call-41",
     "actor":"agent:mira","timeout_ms":20000,"dry_run":false,"depth":1}}
   ```

   Answer with a JSON-RPC response carrying an MCP `CallToolResult`:
   `{"jsonrpc":"2.0","id":"sky-3","result":{"content":[{"type":"text","text":"..."}],"structuredContent":{...},"isError":false}}`,
   or a JSON-RPC error. No answer within `timeout_ms` fails the call with `timeout`.
   `dry_run: true` means a test is running: do not change anything outside the engine.

3. **Callbacks** (client to engine, while serving a call): an ordinary `tools/call` with the call
   id in `_meta`:

   ```json
   {"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"scene_query","arguments":{"tag":"enemy"},
    "_meta":{"skywalker/call_id":"call-41"}}}
   ```

   It is checked against the tool's `capabilities.calls`, `mutate` and `max_calls`, attributed to
   the original caller, and counted for the depth limit. It is served at once even while the
   connection's other requests wait. An unknown or finished call id fails with `unknown_call`.
   Calls without the call id are the client's own calls, under its own identity.

4. **Unregister**: `skywalker/tools/unregister {names:[...]}` or `{all:true}`; `forget:true`
   also deletes persisted definitions. `skywalker/tools/list` returns the client's tools.

When the connection closes, its tools disappear (`persist:false`) or stay listed as `offline`
(`persist:true`, saved in `tools/`); registering the same definition again later does not need
a new approval.

For the polling transport, each call fetched with `tool_host_poll` carries the same `call_id`;
pass it in `_meta["skywalker/call_id"]` on callbacks. `tool_host_register` accepts
`capabilities` and `limits` per tool and returns each tool's status.

## Discovery

- `tools/list` shows custom and external tools with `_meta["skywalker/origin"]` (`custom` or
  `external`), `skywalker/kind`, `skywalker/version` and `skywalker/author`.
- `skywalker tools --json --project DIR` lists them too (`check_skills.py` uses the engine's own).
- The crew offers active custom tools like any other tool, under their category's permission.
- `tool_list_custom` and `tool_inspect` show everything else: pending tools, errors, stats.

## Recipes

### A read-only query: enemies within a radius with their health

```text
tool_define {name:"enemies_near", category:"scene",
  description:"Enemies (tag enemy) within a radius of a point, with their hp. Example: {\"radius\": 8, \"center\": [0,0,0]}",
  input_schema:{type:"object", properties:{radius:{type:"number", default:10}, center:{type:"array", items:{type:"number"}}}},
  wander:"fn run(args)\n  let c = (0, 0, 0)\n  if args.center then c = args.center end\n  let out = []\n  for e in find_all(\"enemy\")\n    if distance(e, c) <= args.radius then out.push({id: e.id, name: e.name, hp: e.hp}) end\n  end\n  return {count: out.length, enemies: out}\nend",
  tests:[{name:"finds one", setup:[{tool:"entity_create", args:{name:"G", tags:["enemy"], vars:{hp:2}}}], args:{radius:5}, expect:{result:{count:{"$gte":1}}}}]}
user_enemies_near {radius: 8}
```

### A composite: place N props along a spline

A pure Wander helper computes the points (`read_scene: false`); a composite places the props
in one undo step.

```text
tool_define {name:"spline_points",
  description:"Evenly spaced points along a smooth curve (Catmull-Rom) through control points. Example: {\"points\": [[0,0,0],[10,0,5],[20,0,0]], \"count\": 8}",
  input_schema:{type:"object", properties:{points:{type:"array", items:{type:"array", items:{type:"number"}}}, count:{type:"integer", default:10}}, required:["points"]},
  capabilities:{read_scene:false},
  wander:"fn catmull(p0, p1, p2, p3, t)\n  let t2 = t * t\n  let t3 = t2 * t\n  return (p1 * 2 + (p2 - p0) * t + (p0 * 2 - p1 * 5 + p2 * 4 - p3) * t2 + (p1 * 3 - p0 - p2 * 3 + p3) * t3) * 0.5\nend\n\nfn run(args)\n  let pts = args.points\n  let n = pts.length\n  if n < 2 then tool_fail(\"give at least 2 control points\") end\n  let out = []\n  for i in 0..args.count\n    let u = 0\n    if args.count > 1 then u = i / (args.count - 1) * (n - 1) end\n    let seg = min(floor(u), n - 2)\n    out.push(catmull(pts[max(seg - 1, 0)], pts[seg], pts[seg + 1], pts[min(seg + 2, n - 1)], u - seg))\n  end\n  return {points: out}\nend",
  tests:[{args:{points:[[0,0,0],[10,0,0]], count:3}, expect:{result:{points:[[0,0,0],[5,0,0],[10,0,0]]}}}]}

tool_define {name:"props_along_spline",
  description:"Places N copies of a primitive along a smooth path through control points, as one undo step. Example: {\"points\": [[0,0,0],[10,0,6],[20,0,0]], \"count\": 12, \"mesh\": \"cylinder\", \"name\": \"Lamp\"}",
  input_schema:{type:"object", properties:{points:{type:"array", items:{type:"array", items:{type:"number"}}}, count:{type:"integer", default:8}, mesh:{type:"string", default:"cube"}, name:{type:"string", default:"Prop"}}, required:["points"]},
  capabilities:{mutate:true},
  steps:[{id:"path", tool:"user_spline_points", args:{points:"{{args.points}}", count:"{{args.count}}"}},
         {id:"made", tool:"entity_create", for_each:"{{steps.path.result.points}}", as:"p", args:{name:"{{args.name}} {{index}}", mesh:"{{args.mesh}}", position:"{{p}}"}}],
  result:{created:"{{steps.made.length}}"},
  tests:[{args:{points:[[0,0,0],[10,0,0]], count:4}, expect:{result:{created:4}}}]}
```

The second tool creates entities, so it waits for a human: `tool_approve {name:"user_props_along_spline"}`.

### A mutating tool that needs approval: rebalance pickups by rarity

```wander
-- tools/rebalance_pickups.wander  (capabilities: {mutate: true})
fn run(args)
  let values = {common: 1, rare: 5, epic: 20}
  let changed = 0
  for p in find_all("pickup")
    let v = 0
    if p.rarity then v = values.get(p.rarity, 0) end
    if v == 0 then
      tool_warn("{p.name} has no known rarity; left as is")
    elif p.value != v then
      p.value = v
      changed += 1
    end
  end
  return {changed: changed}
end
```

Defined with `tool_define {name:"rebalance_pickups", capabilities:{mutate:true}, wander:..., tests:[...]}`
it is saved as `pending_approval`; after `tool_approve`, every call is one undo step attributed
to the caller (`history {action:"undo"}` reverts it).

## Limitations

- Wander tools are synchronous on the engine's main thread; long work belongs in an external
  tool. A call cannot be interrupted mid-computation: the instruction budget bounds it, and wall
  time is checked between nested calls.
- Tests roll back scene edits and skip `write_file`, but tools they call that act outside the
  scene (saving files, importing assets) still act. Built-in tools with such effects modify the
  project, so they already require the `mutate` capability.
- Composite templates are paths, not expressions: computation belongs in a Wander step.
- `wander_check` and `skywalker check` compile gameplay scripts and do not know the tool-only
  builtins (`call_tool`, `read_file`...); check tool code with `tool_test`.
- Approvals and the policy are files in the project; anything that can write project files
  outside the engine can change them (see above).
