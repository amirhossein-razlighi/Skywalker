# Custom tools

When no engine tool does what an agent needs (a query specific to this game, a multi-step edit it keeps repeating, a
bridge to its own process), it can define a **custom tool**. From then on the tool is an ordinary tool: it appears in
`tools/list` (MCP clients get a `notifications/tools/list_changed`), the in-editor crew is offered it, `batch` can call
it, and every call is validated against its schema, attributed to the caller and logged.

The same rules keep every custom tool safe:

- **Least privilege.** A tool declares its capabilities as an allowlist: reading the scene, editing it, the tools it
  may call, the project folders it may read or write, the network. A call runs with the caller's permissions
  intersected with those capabilities.
- **Limits.** An instruction budget, wall time, output size, the number of nested calls, and nesting depth (at most 8
  custom tools deep).
- **Undo.** A call is one undo step attributed to whoever called the tool, and a failing call leaves nothing behind.
- **Approval.** A per-project policy decides which tools a human must approve before they run. An approval is bound to
  the definition's hash, so changing the tool needs approval again.
- **Audit.** Definitions, approvals, registrations and every call are appended to
  `.skywalker/logs/custom_tools.jsonl` and shown in the activity feed.

## Concepts

### Kinds

| Kind | Implementation | Use it for |
|---|---|---|
| `wander` | Wander code with `fn run(args) ... end`, run in-process | Queries and computations over the scene; edits that need logic |
| `composite` | A pipeline of tool calls with templated arguments, loops and a result mapping | Repeating a known sequence of tool calls; the safest kind |
| `external` | Served by a connected client, such as the [Python agent layer](python.md) | Anything that needs a library, a model or the network |

Names are namespaced so they never shadow engine tools: project tools are `user_<name>`, and tools served by a client
are `<namespace>_<name>` (`py_` for the Python layer).

### Status

| Status | Meaning |
|---|---|
| `active` | Callable |
| `pending_approval` | Waits for a human (it mutates, writes files or reaches the network, or the policy is `ask`) |
| `rejected` | A human rejected this definition |
| `disabled` | Turned off with `tool_enable`; files and approval are kept |
| `offline` | An external tool whose client is not connected |
| `invalid` | The file does not parse or validate; the error is in `reason` |
| `policy_off` | The project's policy is `off` |

### The definition

`tool_define` saves a tool as `tools/<name>.tool.json`, with Wander code in `tools/<name>.wander` next to it. The files
are plain JSON and Wander, meant to be committed and reviewed like code.

| Field | Notes |
|---|---|
| `name` | Lowercase letters, digits and underscores; `user_` is added if missing |
| `description` | For language models: what it does, when to use it, key arguments, an example |
| `input_schema` | A JSON Schema object, closed by default so misspelled arguments get did-you-mean errors; `default` values are filled in before the call |
| `output_schema` | Optional; a result that does not match fails the call |
| `capabilities` | `read_scene` (default true), `mutate` (default false), `calls` (tools it may call, names or globs), `files` (`read` and `write` folders inside the project), `network` (external tools only) |
| `limits` | `instructions` (default 1,000,000), `timeout_ms` (5,000; 30,000 for external tools), `max_output_bytes` (65,536), `max_calls` (100) |
| `wander`, or `steps` and `result` | The implementation |
| `tests` | `{name, setup, args, expect}`; they run as dry runs whenever the tool is defined, and failing tests block the save |

`tools/`, `game.json`, `.skywalker/`, `agents/` and `studio/` are never writable by a tool.

### Policy and approval

The policy lives in `game.json` (`{"customTools": {"policy": "auto"}}`):

| Policy | Read-only tools | Tools that mutate, write files or reach the network |
|---|---|---|
| `off` | disabled | disabled |
| `ask` | need approval | need approval |
| `auto` (default) | run at once | need approval |
| `trust` | run at once | run at once |

Only people approve: in the editor's **Studio › Tools** tab, or with `skywalker call` in a terminal. Agents that try
get `approval_requires_human` with instructions for the human. Agents may make the policy stricter with `tool_policy`,
never looser, and `game_settings` refuses to change `customTools` for an agent.

## How to define a tool

### A read-only Wander tool

Enemies within a radius of a point, with their health. The test creates an enemy, runs the tool and rolls everything
back.

```tool
tool_define {"name": "enemies_near", "category": "scene", "description": "Enemies (tag enemy) within a radius of a point, with their hp. Example: {\"radius\": 8, \"center\": [0, 0, 0]}", "input_schema": {"type": "object", "properties": {"radius": {"type": "number", "default": 10}, "center": {"type": "array", "items": {"type": "number"}}}}, "wander": "fn run(args)\n  let c = (0, 0, 0)\n  if args.center then c = args.center end\n  let out = []\n  for e in find_all(\"enemy\")\n    if distance(e, c) <= args.radius then out.push({id: e.id, name: e.name, hp: e.hp}) end\n  end\n  return {count: out.length, enemies: out}\nend", "tests": [{"name": "finds one", "setup": [{"tool": "entity_create", "args": {"name": "G", "tags": ["enemy"], "vars": {"hp": 2}}}], "args": {"radius": 5}, "expect": {"result": {"count": {"$gte": 1}}}}]}
```

It is read-only, so it is `active` at once and callable as `user_enemies_near {"radius": 8}`. The code, as it is saved
in `tools/enemies_near.wander`:

```text
fn run(args)
  let c = (0, 0, 0)
  if args.center then c = args.center end
  let out = []
  for e in find_all("enemy")
    if distance(e, c) <= args.radius then out.push({id: e.id, name: e.name, hp: e.hp}) end
  end
  return {count: out.length, enemies: out}
end
```

Tool code can use the whole Wander library plus builtins that only exist inside tools:

| Builtin | Does |
|---|---|
| `call_tool(name, args)` | Calls an allowlisted tool and returns its structured result; fails the tool on error |
| `try_tool(name, args)` | The same, returning `{ok, result}` or `{ok: false, error, message, hint}` |
| `tool_warn(message)`, `tool_fail(message, hint)` | Add a warning to the result; stop with an error (edits are rolled back) |
| `read_file`, `list_files`, `file_exists`, `write_file` | Project files the tool may read or write (writes are skipped in dry runs) |
| `json_parse`, `json_text` | JSON in and out |
| `tool_actor()` | Who called the tool |

Values crossing the boundary: entities become numeric ids, vectors become `[x, y, z]` arrays (and arrays of three
numbers become vectors), colors become `"#rrggbb"`.

### A composite tool

A pipeline of tool calls. `{{expr}}` templates read `args`, `steps.<id>` (a step's structured result), the loop
variable, `index` and `actor`; `for_each` runs a step per element, and `when` skips it.

```tool
tool_define {"name": "tag_by_tag", "description": "Adds a tag to every entity that has another tag, as one undo step. Example: {\"from\": \"enemy\", \"add\": \"hostile\"}", "input_schema": {"type": "object", "properties": {"from": {"type": "string"}, "add": {"type": "string"}}, "required": ["from", "add"]}, "capabilities": {"mutate": true}, "steps": [{"id": "found", "tool": "scene_query", "args": {"tag": "{{args.from}}"}}, {"tool": "entity_update", "for_each": "{{steps.found.matches}}", "as": "m", "args": {"entity": "{{m.id}}", "tags": ["{{args.from}}", "{{args.add}}"]}}], "result": {"tagged": "{{steps.found.matches.length}}"}}
```

It edits the scene, so under the default policy it is saved as `pending_approval` until a person approves it:

```bash
skywalker call tool_list_custom --project my_game
skywalker call tool_inspect '{"name": "user_tag_by_tag"}' --project my_game
skywalker call tool_approve '{"name": "user_tag_by_tag"}' --project my_game
```

After that, each call is one undo step attributed to the caller.

### Tools served by your own process

A connected client can host tools: the engine forwards each call to it and waits. While the client serves a call it
may call back into the engine; those callbacks carry the call's id (`_meta["skywalker/call_id"]`), so they run with
the hosted tool's declared capabilities and the original caller's identity. With the Python agent layer:

```text
@tool(capabilities={"calls": ["scene_query"]}, limits={"timeout_ms": 20000})
async def hazard_audit(min_gap: float = 6.0, *, ctx: ToolContext) -> dict:
    found = await ctx.session.call("scene_query", {"tag": "hazard"})   # allowed: declared in calls
    ...

host = await engine.host_tools([hazard_audit])
host.status        # {"py_hazard_audit": {"status": "active"}}
```

MCP clients can also register tools on their own connection (`skywalker/tools/register`, advertised in `initialize`
under `capabilities.experimental["skywalker/externalTools"]`); the protocol is in
[docs/CUSTOM_TOOLS.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/CUSTOM_TOOLS.md).

## Pitfalls

- **A pending tool is not offered.** Tools waiting for approval are listed by `tool_list_custom`, not by
  `tools/list`. Ask the person to approve them in **Studio › Tools**.
- **Changing a tool needs approval again.** The approval is bound to the definition's hash.
- **`wander_check` does not know tool builtins.** Check tool code with `tool_test`, which compiles it with
  `call_tool`, `read_file` and the rest.
- **Tests roll back the scene, not the world.** Tools a test calls that act outside the scene (saving files, importing
  assets) still act; those already require `mutate`.
- **Read-only means read-only.** A tool without `mutate` that assigns an entity property, a var or a component field
  fails with `capability_denied`.
- **The boundary is the engine's tool surface.** An agent that can also edit files directly can change tool files
  and `tools/approvals.json`; review them in version control.

!!! agent "For agents"

    Define, test, then call; ask the person when approval is needed:

    ```tool
    tool_list_custom {}
    tool_test {"name": "user_enemies_near", "args": {"radius": 8}}
    tool_inspect {"name": "user_enemies_near"}
    tool_enable {"name": "user_enemies_near", "enabled": false}
    tool_policy {"policy": "ask"}
    ```

## Reference

- Tools: [Custom tools](../reference/tools/tools.md), [Agent layer](../reference/tools/agent.md)
- Design: [docs/CUSTOM_TOOLS.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/CUSTOM_TOOLS.md)
