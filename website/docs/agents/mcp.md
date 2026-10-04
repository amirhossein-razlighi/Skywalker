# The MCP server

Skywalker implements the server side of the [Model Context Protocol](https://modelcontextprotocol.io): JSON-RPC 2.0
over stdio or a Unix socket, with tools, resources and prompts. The same server code backs the CLI (`skywalker mcp`),
the editor's agent socket and the standalone player's development socket, so an agent sees the same surface
everywhere.

## Modes

| Mode | Command | Engine | Use when |
|---|---|---|---|
| Auto | `skywalker mcp --auto [--project DIR]` | The running editor if its socket is on, else a headless engine | The default in agent configs |
| Attach | `skywalker mcp --attach [SOCKET]` | The running editor | Watch and co-edit live |
| Headless | `skywalker mcp --project DIR [--scene FILE]` | An in-process engine on the project | CI, batch generation, no GUI |
| Player | `skywalker-player PROJECT --agent-socket PATH` | The running game | Inspect and drive a running build during development |

The editor's socket is `~/.skywalker/editor.sock` (mode 0600, so only your user can connect). In attach mode the CLI
is a thin bridge: it pipes the stdio session to the socket and back.

```bash
skywalker mcp --auto --project ~/Games/coin_run
```

```json
{"mcpServers": {"skywalker": {"command": "/path/to/skywalker", "args": ["mcp", "--auto"]}}}
```

## Protocol

| Method | Notes |
|---|---|
| `initialize` | Protocol versions `2025-11-25`, `2025-06-18`, `2025-03-26`, `2024-11-05` (the client's version if supported, else the newest). Capabilities: tools, resources, prompts, logging. The result carries short `instructions` describing the agent loop. |
| `tools/list` | Every tool with `name`, `title`, `description`, `inputSchema`, `annotations` and `_meta["skywalker/category"]` |
| `tools/call` | Results contain text, `structuredContent` (JSON) and PNG images where the tool returns one (captures, previews, debug maps) |
| `resources/list`, `resources/read`, `resources/templates/list` | Docs, skills, the live tool catalogue, live studio and scene state |
| `prompts/list`, `prompts/get` | Studio roles and workflows |
| `ping`, `logging/setLevel` | |

Unknown tools are reported as a protocol error with a did-you-mean hint. Invalid arguments are a normal tool error
(`isError: true`) with the validation message, so the model can read it and retry.

```json
{"jsonrpc": "2.0", "id": 7, "method": "tools/call",
 "params": {"name": "entity_update", "arguments": {"entity": "Lamp", "components": {"light": {"intensity": 5}}}}}
```

## Resources

All resources are embedded in the binary; no files are needed.

| URI | Content |
|---|---|
| `skywalker://tools` | The live tool catalogue |
| `skywalker://docs/<NAME>` | Every engine design document, e.g. `skywalker://docs/RENDERING`, `skywalker://docs/WANDER` |
| `skywalker://skills/<skill>` | Every `skywalker-*` skill, plus `…/references/<file>` |
| `skywalker://scene/overview` | The live scene outline (JSON) |
| `skywalker://studio/overview`, `/roster`, `/board`, `/feedback`, `/loops` | The live studio (JSON) |

## Prompts

| Prompt | What it starts |
|---|---|
| `role_creative_director`, `role_level_designer`, `role_environment_artist`, `role_gameplay_programmer`, `role_technical_artist`, `role_sound_designer`, `role_playtester`, `role_critic` | Act as that studio role |
| `new_game` | Build a new game from a pitch |
| `look_dev` | Art-direct the current scene |
| `playtest_loop` | Run the playtest → triage → fix → verify loop with subagents |
| `studio_status` | Summarise the studio |
| `studio_setup` | Create a roster for the project |
| `studio_agent {agent}` | A roster member's live brief (system prompt and permitted tools) |

Clients that surface MCP prompts as slash commands get the workflows even without the plugin files.

## Identity and attribution

Every call carries an actor, recorded in the history and the editor's Activity feed:

| Actor | Who |
|---|---|
| `user` | The person in the editor |
| `agent:<name>` | An in-editor crew member or the headless studio runner |
| `mcp:<client>` | An MCP client, named by its `clientInfo.name` |
| `mcp:<client>/<agent id>` | An MCP client acting as a studio roster member (a client name ending in `/<id>`) |

Clients that cannot rename themselves (several Claude Code subagents share one connection) pass `as: "<agent id>"`
on studio calls. An identified member cannot act as another. One-shot CLI calls can attach with an identity too:

```bash
skywalker call studio_inbox '{}' --attach --as claude-code/mira
```

## Threading and safety

The engine is single-threaded by design. The socket server accepts connections on its own threads, but every tool call
is posted as a job and runs on the engine's main thread. Jobs are not pumped while you are dragging a gizmo, so an
agent's edit is never attributed to your gesture. Requests that time out are marked abandoned and can never apply
changes later. Design-app tools run their slow part off the main thread and only touch the engine for the quick
import-and-place step.

## Embedding without MCP

The C API exposes the same surface for any language with a C FFI:

```c
char* result = sky_call_tool(engine, "scene_overview", "{}", "mcp:my-tool");
char* tools  = sky_tools_list(engine);  /* the MCP tools/list payload */
```

See the [C API reference](../reference/capi.md).

!!! agent "For agents"

    Read resources instead of guessing: the docs for a subsystem, the skill for a workflow, the live catalogue for a
    tool you have not used. On a new connection, orient once:

    ```tool
    engine_info {}
    scene_overview {}
    ```
