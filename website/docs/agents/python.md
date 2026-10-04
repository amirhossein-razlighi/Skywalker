# Python agents

`python/` in the repository is **skywalker-agents**, a Python package for building, running and observing teams of AI
agents on Skywalker. It orchestrates; the engine stays the source of truth. The roster, board, feedback, decisions,
messages, loops and the scene all live in the engine and the project, so agents run from Python appear in the editor's
Studio panel and Activity feed exactly like the editor's own crew, attributed by name.

The distribution is `skywalker-agents`, the import `skywalker_agents` and the command `sky-agents`.

## Concepts

| Piece | What it is |
|---|---|
| Engine client | `AsyncEngine` / `Engine`: one MCP connection per agent. Attaches to the running editor, spawns a headless `skywalker serve`, runs `skywalker mcp` over stdio, or uses an in-process fake for tests. Typed wrappers for every engine tool, generated from `skywalker tools --json`. |
| Agents | `Agent(role, ...)` runs a studio roster member: its brief, permitted tools and profile come from the engine. Claude through the official Anthropic SDK by default, or any OpenAI-compatible or local model. |
| Policies | Budgets (tokens, cost, time, tool calls, turns), autonomy and approvals (including approvals answered by a human in the editor's `#approvals` channel), and middleware for logging, redaction, guardrails, caching and record/replay. |
| Communication | Studio threads with typed messages (`request`, `inform`, `handoff`, `result`, ...), request/reply and handoffs, the studio board as a blackboard, and exported agent cards. |
| Shared memory | Agent, team, project and global scopes in SQLite with full-text search and optional embeddings. The same memory is available to engine agents and to coding-agent sessions over MCP. |
| Workflows | Steps in Python or YAML with dependencies, fan-out, retries, timeouts, repeat-until, human approval gates, checkpoints and resume. Patterns: director and critic, debate, plan and execute, map-reduce, and the playtest, triage, fix and verify loop. |
| Observability | OpenTelemetry GenAI spans for workflows, agent runs, model calls and tool calls; a `trace.jsonl` per run; cost accounting per agent and model; `sky-agents trace` to read them. |
| Extending | Plugins (installed packages with one entry point) add tools, model providers, memory stores, patterns, roles, hooks, approvers and eval checks. Thin adapters expose Skywalker tools to other agent frameworks. |

## Quick start

```bash
cmake --preset release && cmake --build build/release -j 4 --target skywalker   # the engine CLI
cd python && uv sync                                  # Python 3.10 or newer
uv run sky-agents doctor                              # finds the binary, the editor socket and keys (presence only)
uv run python examples/playtest_triage_fix/run.py --dry-run    # scripted models: no API key, a few seconds
uv run python examples/playtest_triage_fix/run.py --attach     # on the project open in the editor, with a real model
```

```python
import asyncio
from skywalker_agents import Agent, AsyncEngine, Memory

async def main():
    async with await AsyncEngine.auto("examples/sky_dash") as engine:   # the editor if it runs, else `skywalker serve`
        memory = Memory.open("examples/sky_dash")
        mira = Agent("level_designer", engine=engine, memory=memory)
        result = await mira.run("Add a three-platform jump section before the goal and verify it with a capture")
        print(result.text, result.cost_usd)

asyncio.run(main())
```

Synchronous code uses `Engine`, with the same API.

## How to run agents

### Attach, spawn or share an engine

| Constructor | What it does |
|---|---|
| `AsyncEngine.attach()` | Attaches to the running editor (or a `skywalker serve` socket); you watch the agents work in the editor. |
| `AsyncEngine.spawn(project)` | Starts `skywalker serve`, a headless engine that serves the agent socket and exits with your process. |
| `AsyncEngine.auto(project)` | Attach if the editor runs, else spawn. |
| `AsyncEngine.fake()` | An in-process fake with a small studio, for tests and offline work. |

`skywalker serve` lets several clients (Python agents, coding agents through `skywalker mcp --attach`, scripts) share one
headless engine:

```bash
skywalker serve --project my_game --socket ~/.skywalker/my_game.sock
skywalker mcp --attach ~/.skywalker/my_game.sock
```

Each agent gets its own connection named `sky-agents/<id>`, so its edits are attributed to it in the history and the
Activity feed, and studio tools treat it as that roster member.

### Workflows

```python
wf = Workflow("lighting_pass", budget=Budget(max_cost_usd=5))

@wf.step()
async def build(ctx):
    return await ctx.agent("aurora").run(f"Light the canyon: {ctx.inputs['goal']}")

@wf.step(after=["build"], retries=1, timeout=900)
async def review(ctx):
    return await ctx.agent("critic").run(f"Review: {ctx.text('build')}. Start with APPROVED if it meets the goal.")

wf.repeat(until=lambda ctx: ctx.text("review").startswith("APPROVED"), max_iterations=3)
result = await wf.run(engine, inputs={"goal": "warm sunset"}, approver=StudioApprover(engine))
```

The same workflow can be written in YAML and run with `sky-agents run`. Every step result is checkpointed under
`<project>/studio/runs/<run id>/`, next to the run's trace and report, so `--resume <run id>` continues after the last
finished step. `--record` and `--replay` make a workflow a deterministic regression test: the replay makes no model
calls and no engine edits.

### Shared memory with coding agents

`sky-agents serve-mcp` serves the project's memory as MCP tools. Add it to a coding agent and give Python agents
`Memory.open(".")`: facts one side remembers, the other recalls.

```bash
claude mcp add sky-memory -- sky-agents serve-mcp --project . --agent claude-code
```

## The engine side

A few engine tools make the Python layer, and any other harness, first-class. They are ordinary tools usable from any
MCP client, and they are *quiet*: they do not appear in the Activity feed or in the crew's tool lists.

| Tool or command | What it does |
|---|---|
| [`events_poll`](../reference/tools/agent.md#events_poll) | Reads the engine's event log (everything the Activity feed sees, numbered, the last 4096 kept) after a cursor, with optional long polling. |
| [`tool_host_register`](../reference/tools/agent.md#tool_host_register) | A process offers tools to every client of the engine as `py_<name>`, each with its capabilities and limits; the result says which are `active` and which wait for approval. |
| [`tool_host_poll`](../reference/tools/agent.md#tool_host_poll), [`tool_host_reply`](../reference/tools/agent.md#tool_host_reply) | The host fetches queued calls and answers them. Hosts that stop polling are dropped with their tools. |
| [`tool_host_unregister`](../reference/tools/agent.md#tool_host_unregister), [`tool_host_list`](../reference/tools/agent.md#tool_host_list) | Stop serving; list who serves what. |
| [`studio_presence`](../reference/tools/studio.md#studio_presence) | Live status (working, idle, waiting, blocked) in the Studio panel for agents run outside the editor. |
| `skywalker serve` | A headless engine serving the agent socket like the editor. |

```tool
events_poll {"since": 0, "types": ["studio.message"], "limit": 50}
studio_presence {"status": "working", "activity": "Lighting the canyon"}
tool_host_list {}
```

## Pitfalls

- **New `py_*` tools need a reconnect** in clients that cache the tool list; the engine does not push list changes.
  `sky-agents serve-mcp` is the better path for shared memory in those sessions.
- **Hosted tools need a socket connection.** A call made on the engine's own thread (stdio `skywalker mcp`, the CLI
  runner) fails fast with a hint instead of waiting for the host.
- **Regenerate the typed wrappers** with `uv run sky-agents gen-tools` after engine tools change; an integration test
  fails when they drift.
- **Hosted tools follow the custom tool rules.** Each declares the engine tools it calls back into
  (`@tool(capabilities={"calls": ["scene_query"]})`); callbacks made through `ctx.session` while serving a call carry
  its id and are checked against that list. Tools that mutate wait for a person's approval under the default policy
  (`host.status`, `host.pending()`). See [Custom tools](custom-tools.md).
- **The event log is bounded.** A reader more than 4096 events behind gets `truncated: true` and continues.

!!! agent "For agents"

    You do not need the Python package to use the engine side: any MCP client can follow the studio with
    `events_poll`, report its status with `studio_presence`, and serve its own tools with `tool_host_register`.

## Reference

- Tools: [Agent layer tools](../reference/tools/agent.md), [Studio tools](../reference/tools/studio.md)
- CLI: [`skywalker serve`](../reference/cli.md#serve)
- Design: [docs/PYTHON_AGENTS.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/PYTHON_AGENTS.md),
  [python/README.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/python/README.md)
