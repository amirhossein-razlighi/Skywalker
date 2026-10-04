---
name: skywalker-custom-tools
description: Define your own Skywalker tools when no engine tool does what you need - Wander tools (fn run(args) over the scene with call_tool), composite pipelines of tool calls with templates and loops, tests as dry runs, capabilities (read_scene, mutate, calls, files), limits, the approval policy (tool_approve is for humans), tool_inspect for debugging, the user library, and hosting tools from your own process over MCP (skywalker/tools/register, call ids for callbacks). Use when you repeat the same multi-step edit, need a game-specific query, or want to serve tools from a Python process.
---

# Custom tools

Load skywalker-core first. Engine doc: `skywalker://docs/CUSTOM_TOOLS`. A custom tool is a real engine tool: it
appears in `tools/list` as `user_<name>` (clients get `notifications/tools/list_changed`), the crew can use it, and
every call is validated, attributed, logged and undoable.

## When to define one

- You are about to make the same 3+ tool calls again (a composite makes it one call and one undo step).
- You need a question answered that `scene_query` cannot (distances, sums, health of nearby enemies, a report).
- A team will need it again: tools live in the project (`tools/<name>.tool.json`) and are committed with it.
Do not define a tool for a one-off edit: use `batch`.

## The loop

1. **Draft and try without saving**: `tool_test {definition:{...}, args:{...}}` runs it as a dry run (edits rolled back).
2. **Define with tests**: `tool_define {...}` validates, compiles, runs the tests (dry runs; failing tests block the save),
   saves and registers. Read `status` in the result.
3. **Call it** like any tool (`user_<name> {...}`).
4. **Debug**: `tool_inspect {name}` (code, status and why, approval, stats, recent calls and errors), then `tool_define`
   again with only the fields you change (they are merged over the current definition).

```text
tool_define {name:"enemies_near", category:"scene",
  description:"Ids of enemies (tag enemy) within a radius of the origin. Example: {\"radius\": 8}",
  input_schema:{type:"object", properties:{radius:{type:"number", default:10}}},
  wander:"fn run(args)\n  let out = []\n  for e in find_all(\"enemy\")\n    if distance(e, (0, 0, 0)) <= args.radius then out.push(e.id) end\n  end\n  return out\nend",
  tests:[{name:"finds one", setup:[{tool:"entity_create", args:{name:"G", tags:["enemy"]}}], args:{radius:5}, expect:{result:{"$len":{"$gte":1}}}}]}
tool_inspect {name:"user_enemies_near"}
```

## Wander tools

Code is a file of `fn`/`const`/`use` declarations with a top-level `fn run(args)`; what it returns is the result.
Everything in Wander works (`find_all`, `nearest`, `distance`, entity properties and vars, component fields), plus:
`call_tool(name, args)` (fails the tool on error), `try_tool` (returns `{ok, result|error}`), `tool_warn`, `tool_fail(message, hint)`,
`read_file` / `write_file` / `list_files` / `file_exists` (declared folders only), `json_parse`, `json_text`, `tool_actor`.
Entities cross the boundary as ids (`find("#12")` gets one back); `[x, y, z]` arrays become vectors.

## Composite tools

```text
tool_define {name:"tag_by_tag", description:"Adds a tag to every entity that has another tag, as one undo step. Example: {\"from\": \"enemy\", \"add\": \"hostile\"}",
  input_schema:{type:"object", properties:{from:{type:"string"}, add:{type:"string"}}, required:["from", "add"]},
  capabilities:{mutate:true},
  steps:[{id:"found", tool:"scene_query", args:{tag:"{{args.from}}"}},
         {tool:"entity_update", for_each:"{{steps.found.matches}}", as:"m", args:{entity:"{{m.id}}", tags:["{{args.from}}", "{{args.add}}"]}}],
  result:{tagged:"{{steps.found.matches.length}}"}}
```

`"{{expr}}"` alone keeps the type; inside text it is interpolated. Roots: `args`, `steps.<id>`, `prev`, the loop variable,
`index`, `actor`; `.length`, `[n]` and `?? default` work. `when` skips a step; `continue_on_error` keeps going. Templates are
paths, not math: compute in a Wander tool and call it from a step (docs: "place N props along a spline").

## Capabilities: ask for the least

| Capability | Default | Note |
|---|---|---|
| `read_scene` | true | false = pure computation on an empty scene |
| `mutate` | false | edit the scene (directly or via mutating tools); needs a human's approval |
| `calls` | [] | tools it may call (globs ok); composites derive it from their steps |
| `files` | none | `{read:["data/"], write:["exports/"]}`; never `tools/`, `game.json`, `.skywalker/`, `agents/`, `studio/`; writing needs approval |

Limits: `instructions` (1e6), `timeout_ms` (5000), `max_output_bytes` (64 KB), `max_calls` (100). Custom tools may nest 8 deep.

## Approval: you cannot approve

Under the default policy (`auto`) read-only tools run at once; tools that mutate or write files are saved as
`pending_approval` and not callable until a **human** approves them. You get `approval_requires_human` if you try.
Tell the human what the tool does, its capabilities and how to approve:
the editor's Studio > Tools panel, or `skywalker call tool_approve '{"name":"user_x"}' --project DIR`.
Changing the tool later needs approval again. `tool_list_custom` shows statuses and the policy; you may make the policy
stricter with `tool_policy`, never looser. Your own permissions still apply inside a tool.

## Hosting tools from your own process

Over the MCP connection your client already has (stdio or socket):

```text
{"jsonrpc":"2.0","id":7,"method":"skywalker/tools/register","params":{"namespace":"py","tools":[
  {"name":"memory_recall","description":"Search the team's shared memory for notes about a topic",
   "inputSchema":{"type":"object","properties":{"query":{"type":"string"}},"required":["query"]},
   "capabilities":{"calls":["scene_query"]}}]}}
```

The engine then sends you `skywalker/tools/call` requests (`{name, arguments, call_id, actor, timeout_ms, dry_run}`); answer
with a `CallToolResult`. To call back into the engine while serving one, send `tools/call` with
`_meta: {"skywalker/call_id": "<call_id>"}`: it runs with the tool's `capabilities.calls`. Tools disappear when you
disconnect (`persist:true` keeps them listed as offline). The polling alternative is `tool_host_register` (see skywalker-studio).

## Pitfalls

- **Keywords are not names**: `fn go(...)` fails to compile (`go` is a keyword); the error names the problem.
- **Read-only tools cannot assign** entity properties or vars (`capability_denied`): declare `mutate:true` or only read.
- **Closed schemas**: arguments not in `input_schema` are rejected with a did-you-mean; add `default`s for optional ones.
- **Big results** fail with `output_too_large`: return ids and counts, take a `limit` argument.
- **Tests are dry runs** but tools they call that act outside the scene still act; keep tests to scene effects.
- Names: lowercase, digits, underscores; `user_` is added. `user.x` and `x` both mean `user_x`.
