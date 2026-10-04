# Python agents (skywalker-agents)

`python/` is a Python package for building, running and observing teams of AI agents on Skywalker. It
orchestrates; the engine stays the source of truth. Roster, board, feedback, decisions, messages, loops and
the scene live in the engine and the project, so agents run from Python show up in the editor's Studio panel
and Activity feed exactly like the editor's own crew, attributed by name.

```
 ┌──────────────── your code / sky-agents CLI ────────────────────────────────────────────────┐
 │ Workflow (steps, repeat, checkpoints, budgets) ── patterns: director/critic, debate,        │
 │   │                                               plan-and-execute, map-reduce, playtest loop │
 │   ├─ Agent "mira" ── Role (agents/mira.agent.json + studio_agent_brief)                     │
 │   │     ├─ Provider: Anthropic SDK (claude-opus-5-5) │ OpenAI-compatible │ LiteLLM │ scripted  │
 │   │     ├─ Tools: engine tools │ @tool functions │ MCP servers │ memory_*                      │
 │   │     ├─ Memory: agent / team / project / global (SQLite + FTS5, vectors optional)          │
 │   │     └─ Policies: budget, autonomy + approvals, middleware (redaction, guardrails, cache)  │
 │   ├─ MessageBus (studio threads), Blackboard (studio board), agent cards                     │
 │   └─ Tracer (GenAI spans) → trace.jsonl, OpenTelemetry, Langfuse, Phoenix; CostMeter         │
 ├────────── AsyncEngine / Engine: one MCP connection per agent ("sky-agents/<id>") ────────────┤
 │  socket: the editor (~/.skywalker/editor.sock) or `skywalker serve`   │ stdio │ FakeEngine     │
 └────────────────────────────────────────────────────────────────────────────────────────────┘
        │ tools/call                 │ events_poll (long poll)          │ tool_host_* (py_* tools)
 ┌──────▼────────────────────────────▼──────────────────────────────────▼──────────────────────┐
 │ Engine: studio_* · playtest_* · scene/entity/render tools · EventLog · ToolHost              │
 └──────────────────────────────────────────────────────────────────────────────────────────────┘
```

The distribution is `skywalker-agents`, the import `skywalker_agents`, the command `sky-agents`. `skywalker` alone
would collide with the engine's own CLI name and with future engine bindings (the C API is callable from Python);
the `-agents` suffix says what the package is, and the import name mirrors the distribution as PEP 8 expects.

## Quick start

```bash
cmake --preset release && cmake --build build/release -j 4 --target skywalker      # the engine CLI
cd python && uv sync                                  # Python >= 3.10; or: pip install -e ".[anthropic]"
uv run sky-agents doctor                              # finds the binary, the editor socket, keys (presence only)
uv run python examples/playtest_triage_fix/run.py --dry-run     # scripted models, no API key, a few seconds
export ANTHROPIC_API_KEY=...                          # or `ant auth login`
uv run python examples/playtest_triage_fix/run.py --attach      # on the project open in the editor: watch it
```

```python
import asyncio
from skywalker_agents import Agent, AsyncEngine, Memory

async def main():
    async with await AsyncEngine.auto("examples/sky_dash") as engine:   # the editor if running, else `skywalker serve`
        memory = Memory.open("examples/sky_dash")
        mira = Agent("level_designer", engine=engine, memory=memory)
        result = await mira.run("Add a three-platform jump section before the goal and verify it with a capture")
        print(result.text, result.cost_usd)

asyncio.run(main())
```

Synchronous code uses `Engine` (same API): `Engine.spawn("examples/sky_dash").tools.scene_overview().text`.

## Concepts

### The engine client

| Constructor | What it does |
|---|---|
| `AsyncEngine.attach(socket=None)` | Attaches to the running editor (`~/.skywalker/editor.sock`, `$SKYWALKER_SOCKET`) or a `skywalker serve` socket. |
| `AsyncEngine.spawn(project, transport="serve")` | Starts `skywalker serve` (a headless engine serving the agent socket; it exits with your process). `transport="stdio"` runs `skywalker mcp` (one connection: no long polls, no tool hosting). |
| `AsyncEngine.auto(project)` | Attach if the editor runs, else spawn. |
| `AsyncEngine.fake()` | An in-process fake with a small studio (tests, offline). |

- `await engine.call("entity_create", {"name": "Pillar"}, check=True)` returns a `ToolResult` (`.text`, `.data`, `.images`,
  `.pil_images()`, `.error_code`, `.error_hint`); `check=True` raises `ToolError` with the engine's code and hint.
- `engine.tools.<name>(...)` are typed wrappers generated from `skywalker tools --json` (pydantic models validate before
  the call; `as` is `as_`). Regenerate after engine tool changes with `sky-agents gen-tools`; an integration test fails when they drift.
- `engine.as_agent("mira")` is a session on its own connection named `sky-agents/mira`: its edits are attributed to
  `mcp:sky-agents/mira`, and studio tools treat it as roster member `mira` (on stdio the `as` argument is added instead).
- `engine.events(types=["studio.message"])` follows the event stream (`events_poll`): an async iterator of `Event`
  (`type`, `kind`, `action`, `actor`, `tool`, `summary`, plus the payload); `await stream.next(timeout=5)` waits with a deadline.
- `await engine.host_tools([tool, ...])` serves Python tools to every client of the engine as `py_<name>` (below).

### Agents

`Agent(role, engine=..., provider=..., tools=..., memory=..., budget=..., approver=..., middleware=..., tracer=...)`.

- **Role.** A roster id: the agent fetches `studio_agent_brief` (the engine's system prompt for that member and the tools it
  may use, with `allow`/`ask` access) and its profile (model, provider, autonomy, max rounds). `Role.load(project, id)` reads
  `agents/<id>.agent.json` offline; `Role.adhoc("judge", instructions=...)` makes an agent that is not on the roster.
- **Model.** The profile's `provider`/`model`, else `claude-opus-5-5` (`claude-sonnet-5-5` for the playtester and QA lead roles).
  `get_provider("anthropic" | "openai" | "ollama" | "lmstudio" | "vllm" | "llamacpp" | "litellm" | "scripted" | <plugin>)`.
- **Loop.** Model turn → tool calls (run concurrently, results returned together) → repeat until the model stops calling
  tools, `max_turns`, or a budget. A turn cut off by `max_tokens` never runs its tool calls (the model is told to re-issue
  them); a `refusal` ends the run and discards the partial output; `pause_turn` is resumed; invalid tool JSON comes back as
  an `INVALID_JSON` error; unknown or withheld tools come back as `not_permitted`.
- **Studio integration.** Presence (`studio_presence` working/idle with the task's first line), per-request token usage
  (`studio_usage_report`), and every engine call on the agent's own attributed connection.
- **Memory.** With `memory=`, the agent gets `memory_*` tools, relevant memories are added to the task prompt (after the
  cached system prompt), and each run is logged as an episodic memory.
- `await agent.chat("...")` keeps the conversation between calls; `result.messages` holds the transcript.

#### Claude specifics (Anthropic SDK)

Streaming requests (`client.beta.messages.stream(...).get_final_message()`, `max_tokens` 64000), adaptive thinking with
an explicit `output_config.effort` (default `high`; Opus 5.5's own default is `medium` and its thinking cannot be
disabled), prompt caching (a breakpoint on the system prompt plus top-level automatic caching), server-side refusal
fallback (`fallbacks: "default"`, beta `server-side-fallback-2026-07-01`) on models and endpoints that support it,
`eager_input_streaming` on tools (inputs are validated by the tools before they run), append-only history (assistant
content, thinking blocks included, is echoed back unchanged), and the SDK's typed errors mapped to retryable or fatal
`ProviderError`s. Credentials come from the environment (`ANTHROPIC_API_KEY`) or `ant auth login`; nothing is stored.

### Policies

- **Budgets.** `Budget(max_tokens, max_cost_usd, max_seconds, max_tool_calls, max_turns)` per agent, per workflow run
  (`Workflow(..., budget=)` or `--max-cost`); agent trackers charge the run's, so a run-wide cap holds across parallel
  agents. Exceeding one ends the run with status `budget`.
- **Autonomy and approvals.** `ApprovalPolicy` asks for approval for tools the brief marks `ask`, every mutating tool of an
  `ask`-autonomy agent, or explicit patterns (`always`, `never`). `observe` agents only get read-only tools. Approvers:
  `AutoApprove`, `AutoDeny` (the default without an approver), `TerminalApprover`, `CallbackApprover(fn)`, and
  `StudioApprover(engine)`, which posts in the editor's `#approvals` channel and waits for a human to answer
  `approve A-1f2e` / `deny A-1f2e reason` there (or in the thread).
- **Middleware** wraps tool calls and model calls: `LoggingMiddleware`, `RedactionMiddleware` (API keys and tokens by
  default, plus your patterns), `GuardrailMiddleware(deny=["asset_download"], max_calls={"playtest_run": 5})`,
  `CacheMiddleware` (identical read-only calls, cleared by any edit), `Recorder`/`Replayer`, or your own `Middleware` subclass.
- **Retries and timeouts.** Model calls retry with exponential backoff (`retry_async`), tool calls time out
  (`tool_timeout`, default 600 s), workflow steps take `retries` and `timeout`; cancellation propagates.

### Communication

- **Messages are studio messages** with a `kind` (`request`, `inform`, `handoff`, `result`, `question`, `approval_request`, ...)
  and a JSON `data` payload; threads persist in `studio/messages.jsonl`, and the editor's Messages view shows every exchange.
- `StudioBus(engine)`: `tell` (direct), `publish` (topic = channel), `subscribe(agent=, topic=, kinds=, thread=)` (fed by the
  event stream), `request` (waits for the first reply in the thread), `reply`, `handoff` (a `handoff` message plus a board
  task with acceptance criteria), `thread(id)`, `inbox(agent)`. `LocalBus` is the same API in memory.
- `Blackboard(engine)`: the studio board as a blackboard: `post`, `claim`, `update`, `complete`, `tasks`, `watch`.
- `sky-agents cards --project DIR --out cards/` exports A2A-style agent cards (name, description, skills, a `skywalker` block
  with role, focus and tools) so external agents can discover the roster.

### Shared memory

| Scope | Visible to | Stored in |
|---|---|---|
| `agent` | that agent (`owner` = agent id) | `<project>/studio/memory.sqlite` |
| `team` | agents in the team (`owner` = team) | same |
| `project` | everyone on the project | same |
| `global` | the user, across projects | `~/.skywalker/agents/memory.sqlite` (`$SKY_AGENTS_HOME`) |

Kinds: `episodic` (what happened; agents log every run), `fact` (durable; duplicates merge), `note` (knowledge, with `links`),
`artifact` (a file: capture, report). Recall ranks by FTS5 keyword relevance, optional embeddings (`HashingEmbedder`,
`SentenceTransformerEmbedder`, `OpenAIEmbedder`; `sqlite-vec` when installed, an exact scan otherwise), recency and importance.
`Memory.remember / recall / forget / summarize / link / log / add_artifact`, maintenance with `consolidate` (old episodes folded
into summary notes, duplicate facts merged) and `decay` (weak, unused items archived; importance >= 0.9 never decays).
`remember(..., pin=True)` also adds the note to the agent's studio memory, which its brief shows in the editor.
Other stores implement the `MemoryStore` protocol (`QdrantMemoryStore` is the reference adapter; register yours as a plugin).

The same memory is available to every agent on the engine (`sky-agents host-tools` → `py_memory_*`) and to Claude Code,
Codex, Cursor or Gemini sessions (`sky-agents serve-mcp`).

### Workflows

Python:

```python
wf = Workflow("lighting_pass", budget=Budget(max_cost_usd=5))

@wf.step()
async def build(ctx):
    return await ctx.agent("aurora").run(f"Light the canyon: {ctx.inputs['goal']}")

@wf.step(after=["build"], retries=1, timeout=900)
async def review(ctx):
    return await ctx.agent("critic").run(f"Review: {ctx.text('build')}. Start with APPROVED if it meets the goal.")

@wf.step(after=["review"], map_over=lambda ctx: ["sky", "rocks", "water"])
async def polish(ctx, part):
    return await ctx.agent("aurora").run(f"Polish the {part}")

wf.repeat(until=lambda ctx: ctx.text("review").startswith("APPROVED"), max_iterations=3)
result = await wf.run(engine, inputs={"goal": "warm sunset"}, approver=StudioApprover(engine))
```

Steps run as soon as their dependencies finish (bounded by `concurrency`); `when` skips; `map_over` fans out; a failed step
fails the run after its retries. `ctx.agent(id)` gives run-configured agents (shared provider, memory, tracer, budget,
approvals, middleware); `ctx.bus`, `ctx.board`, `ctx.memory`, `ctx.state` (persisted), `ctx.result/text/previous`.

YAML (or JSON) for `sky-agents run`: steps are `agent` (prompt template), `tool` (an engine call), `playtest`, `approval`
(a human gate), `pattern` (a registered pattern), `studio_loop` (an engine loop driven by Python agents) or `call`
(`module:function`); `after`, `when`, `map_over`, `retries`, `timeout`; `repeat: {until, max_iterations}`; `budget`,
`defaults` (agent options), `inputs`. Templates (`{{ steps.review.text }}`) and conditions use a small, safe expression
language (no calls on objects, no imports, no private attributes). An engine loop file (`stages`) runs as that Studio loop.
See [python/examples/playtest_triage_fix/workflow.yaml](../python/examples/playtest_triage_fix/workflow.yaml).

**Checkpoints and resume.** Every step result is saved atomically to `<project>/studio/runs/<run id>/checkpoint.json`;
`sky-agents run wf.yaml --resume <run id>` (or `wf.run(resume=...)`) continues after the last finished step. The run's
trace (`trace.jsonl`) and summary (`report.json`) live next to it.

**Patterns** (`skywalker_agents.harness`): `director_critic` (maker and critic, the critic sees a capture),
`debate` (rounds and a judge), `plan_and_execute` (a JSON plan becomes board tasks for executors), `map_reduce`,
`playtest_triage_fix_verify` (playtest, file findings, director triage with `studio_decide`, fixers, verification with
`playtest_compare`), and `run_studio_loop` (drives `studio_loop_start` / `studio_loop_advance`: the engine runs playtest
stages and measures effects, Python agents execute the assignments, approvals go to the approver).

**Record and replay.** `Recorder("golden.cassette.jsonl")` as middleware records every model response and tool result;
`Replayer(path)` (middleware) with `provider=replayer.provider()` replays them: no model calls, no engine edits, identical
results, and a strict replay fails at the first divergence. `sky-agents run wf.yaml --record/--replay`.

**Evals.** `run_eval(subject, scenarios, engine_factory=...)` runs a workflow per scenario (fresh engine each) and scores it
with checks: `tool_check(..., expect="len(data.entities) > 0")`, `sim_trace_check`, `metric_check("deaths", max=1)`
(plays the game), `vision_check(rubric, provider)` (a capture judged by a vision model), `text_check`. `sky-agents eval spec.yaml`.

### Observability

Spans for workflows, steps, agent runs (`invoke_agent`), model calls (`chat`) and tool calls (`execute_tool`) carry the
OpenTelemetry GenAI attributes (`gen_ai.operation.name`, `gen_ai.provider.name`, `gen_ai.request.model`,
`gen_ai.usage.input_tokens`, `gen_ai.usage.output_tokens`, cache tokens, `gen_ai.tool.name`, `gen_ai.agent.id`, ...)
plus `skywalker.cost_usd`. Every run writes `trace.jsonl`; `sky-agents trace list | show RUN|latest [-v] | stats` reads it.
Export with `Tracer([OTelSink(configure_otel(endpoint))])`, `OTelSink(langfuse())` or `OTelSink(phoenix())`
(`--otel ENDPOINT` on the CLI). Prompt and completion text stays out of exported spans unless `capture_content=True`.
`CostMeter` totals tokens and USD per agent and model (Claude prices built in; `set_price` for others).

## Extending

Everything is a registry entry, and plugins are installed packages with one entry point:

```toml
[project.entry-points."skywalker_agents.plugins"]
my_plugin = "my_plugin:register"
```

```python
def register(registry):
    registry.tools.add("hazard_audit", hazard_audit)                  # a Tool (@tool function)
    registry.providers.add("my-llm", lambda **kw: MyProvider(**kw))   # anything with complete(LLMRequest)
    registry.memory_stores.add("my-store", MyStore)                   # MemoryStore protocol
    registry.embedders.add("my-embedder", MyEmbedder)
    registry.patterns.add("fairness_review", fairness_review)          # async fn(ctx, **args): usable from YAML
    registry.roles.add("hazard_designer", {...})                       # a studio_agent_define template
    registry.hooks.add("spend_limit", SpendLimit)                      # Middleware
    registry.approvers.add("slack", SlackApprover)
    registry.checks.add("my_check", my_check)                         # eval checks
```

`sky-agents new plugin NAME` scaffolds a package with a tool, a hook, a pattern and a test; `sky-agents plugins` lists
everything registered (and plugins that failed to load). A complete example is
[python/examples/custom_plugin](../python/examples/custom_plugin).

| To add | Write |
|---|---|
| A tool | `@tool` on a (sync or async) function: the schema comes from the signature, descriptions from the docstring's `Args:`; a `ctx: ToolContext` parameter receives the caller (agent id, its engine session, memory). Return str, dict, a pydantic model, PNG bytes or a `ToolResult`. |
| A provider | A class with `name`, `default_model`, `supports_vision` and `async complete(LLMRequest) -> LLMResponse` (see `llm/scripted.py`). Keep assistant `raw` content to echo it back. |
| A memory store | The `MemoryStore` protocol (`memory/base.py`): add, get, update, delete, search, list_items, touch, stats, close. |
| A pattern | `async def pattern(ctx: RunContext, **args) -> dict`. |
| A hook | Subclass `Middleware`, override `on_tool` and/or `on_llm`, call `call_next`. |
| MCP servers as tools | `async with await MCPToolset.stdio("npx", [...]) as fs: Agent(..., tools=fs.tools)`. |

### Other frameworks

Thin adapters turn Skywalker tools (engine tools via `adapters.engine_tools(engine, names, agent=...)`, memory tools, your
`@tool`s) into each framework's tool type: `adapters.langchain.as_langchain_tools` (LangChain and LangGraph),
`adapters.openai_agents.as_openai_agents_tools` (or `skywalker_mcp_params()` for the whole engine as an MCP server),
`adapters.pydantic_ai.as_pydantic_ai_tools`, `adapters.crewai.as_crewai_tools`, and for the Claude Agent SDK
`adapters.claude_agent_sdk.skywalker_mcp_config()` plus `as_sdk_mcp_server(tools)`. Example:
[python/examples/langgraph_adapter.py](../python/examples/langgraph_adapter.py).

## The engine side: the agent link

Small engine additions make the Python layer (and any other harness) first-class. They are ordinary tools, usable from
any MCP client.

| Tool / command | What it does |
|---|---|
| `events_poll {since, types, actors, exclude_actors, limit, wait_ms}` | Reads the event log (every event the Activity feed sees, numbered by `seq`, the last 4096 kept) after a cursor, without consuming it. `wait_ms` long-polls on agent socket connections (the wait runs on the connection's thread, never the engine's). |
| `tool_host_register {tools, host, label, ttl_seconds, replace}` | A process offers tools to every client as `py_<name>` (names valid for every model API). |
| `tool_host_poll {host, wait_ms, max}` / `tool_host_reply {host, call, text, structured, images, is_error}` | The host fetches queued calls and answers them; the caller's connection waits up to 120 s. Hosts that stop polling for `ttl_seconds` are dropped with their tools. |
| `tool_host_unregister {host}` / `tool_host_list` | Stop serving; who serves what, queue sizes, the event cursor range. |
| `studio_presence {status, activity, as}` | Live status in the Studio panel for agents run outside the editor. |
| `studio_message_send {kind, data}`, `studio_inbox {thread, after}` | Structured messages and whole threads. Message events carry the full message. |
| `skywalker serve --project DIR [--socket PATH] [--lifeline]` | A headless engine that serves the agent socket like the editor; several clients share it. |

These tools are marked *quiet*: they do not appear in the Activity feed or the crew's tool lists (registration and removal of
tool hosts emit their own `tool_host` events). `py_*` tools are callable from agent socket connections and the editor's
crew; a call made on the engine's own thread (stdio `skywalker mcp`, the CLI runner) fails fast with a hint instead of waiting.

## Recipes

**Overnight playtest loop on the editor's project.**
`sky-agents run python/examples/playtest_triage_fix/workflow.yaml --attach --max-cost 10`; approvals arrive in `#approvals`.

**Claude Code and Python agents sharing memory.**
`claude mcp add sky-memory -- sky-agents serve-mcp --project . --agent claude-code`, and give Python agents
`Memory.open(".")`. Facts one side remembers, the other recalls.

**Drive an engine loop with a cheaper model for one role.**
`studio_agent_define {id: "playtester", model: "claude-sonnet-5-5"}` then
`await run_studio_loop(engine, "playtest_fix_verify", ctx, define={"template": "playtest_fix_verify"})` inside a workflow
step; each agent uses its profile's model.

**Deterministic regression test of an agent workflow.** Record once with `--record golden.jsonl` (live models), commit the
cassette, and run `--replay golden.jsonl` in CI: no keys, no cost, identical tool traffic.

**Local models.** `OLLAMA_MODEL=qwen3 sky-agents chat mira --provider ollama` (or `lmstudio`, `vllm`, any
`<NAME>_BASE_URL`); vision falls back to text when the model has none (`OPENAI_VISION=0`).

**Watch what agents do from a script.**
`async for ev in engine.events(types=["tool"], actors=["mcp:sky-agents/"]): print(ev.actor, ev.tool, ev.summary)`.

## Development

```bash
cd python
uv sync                                   # dev tools included
uv run pytest -q                          # unit tests (fake engine, scripted models) + integration tests when a binary is built
uv run pytest -m "not integration"        # what CI runs
uv run mypy && uv run ruff check . && uv run ruff format --check .
SKY_AGENTS_LIVE=1 uv run pytest tests/live   # one real Claude call (needs ANTHROPIC_API_KEY)
uv run sky-agents gen-tools               # after changing engine tools
```

Unit tests never need API keys: models are `ScriptedProvider`s (the same format as `skywalker studio run --mock`) and the
engine is `FakeEngine` or a real `skywalker serve` on a throwaway copy of a project.

## Limitations

- `py_*` tools appear in `tools/list` when registered, but the engine does not push `notifications/tools/list_changed`:
  clients that cache the list (Claude Code) see new `py_*` tools after reconnecting. `sky-agents serve-mcp` is the
  better path for shared memory in those sessions.
- Approvals in the editor use the `#approvals` channel; there are no dedicated buttons yet.
- The event log keeps the last 4096 events; a reader that falls further behind gets `truncated: true` and continues.
- Memory search is keyword + optional embeddings in one SQLite file per project; large teams or cross-machine sharing want a
  server store (the Qdrant adapter, or your own `MemoryStore`).
- The CrewAI, Pydantic AI and OpenAI Agents SDK adapters are thin and tested at the tool level only; LangGraph is exercised by an example.
