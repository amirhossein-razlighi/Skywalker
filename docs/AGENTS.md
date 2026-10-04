# Agents in Skywalker

There are three ways an AI works with Skywalker, and all three use the same
[tool surface](TOOLS.md), which agents can extend with [their own tools](CUSTOM_TOOLS.md):

1. **External agents over MCP.** Claude Code, Codex, Cursor, Gemini CLI, or any MCP
   client.
2. **The studio and its crew ("Cloudlings").** A roster of specialist agents with a task
   board, feedback, director decisions, playtest bots and loops. It lives in the engine and
   the project, so the editor's crew, the headless runner (`skywalker studio run`) and
   external agents all share it. See [STUDIO](STUDIO.md).
3. **Generators.** Image, 3D, audio, music and video models that fulfill asset requests.

Your own agent programs use the same surface; the [Python agent layer](PYTHON_AGENTS.md) (`python/`) packages it:
a typed client, agents, shared memory, workflows, approvals, evals and tracing.

## 1. MCP

Skywalker implements the [Model Context Protocol](https://modelcontextprotocol.io) server
side: `initialize` (protocol versions 2025-11-25 → 2024-11-05), `tools/list`, `tools/call`
(text, structured content, and PNG images), `ping`, `resources/list|read|templates/list` (the docs, the `skywalker-*` skills, the live tool catalogue, live studio and scene state) and
`prompts/list|get` (studio roles and workflows). See [INTEGRATIONS](INTEGRATIONS.md) for skills, subagents and `skywalker setup`.

| Mode | Command | Use when |
|---|---|---|
| Auto | `skywalker mcp --auto [--project DIR]` | Default for agent configs: attach to the editor if it runs, else a headless engine. |
| Attach to the editor | `skywalker mcp --attach [socket]` | You want to watch and co-edit live. Edits appear in the activity feed as `mcp:<client>`. |
| Headless | `skywalker mcp --project DIR [--scene FILE]` | CI, batch generation, or no GUI. |
| Shared headless | `skywalker serve --project DIR [--socket PATH]` | Several agents on one headless engine: they attach to its socket like to the editor (`mcp --attach PATH`). |

The editor's socket (`~/.skywalker/editor.sock`, mode 0600) starts with the editor. Toggle
it from the status bar or Settings → External Agents.

```bash
claude mcp add skywalker -- /path/to/skywalker mcp --auto     # or: skywalker setup claude|codex|gemini|cursor
```

Generic MCP config (Cursor, Gemini CLI, …):

```json
{ "mcpServers": { "skywalker": { "command": "/path/to/skywalker", "args": ["mcp", "--attach"] } } }
```

### A good agent loop

1. `engine_info`, then `scene_overview`: learn the conventions and what exists.
2. Act with `entity_create`, `entity_update`, `transform`, `environment_update`, or
   `batch` for many edits (atomic, one undo step).
3. Look with `viewport_capture` (`annotate: true` labels every entity with its `#id`;
   the structured result lists screen boxes).
4. For behaviors: read `wander_reference` once; derive spec rules from the intent; write the
   code with a `test` block per rule; `wander_check`, `behavior_set` (with `spec`),
   `wander_test` until green, `behavior_spec` for coverage; then `sim_control step`,
   `sim_input`, `logs`, and finally `sim_control stop` (see [WANDER.md](WANDER.md)).
5. If something went wrong, `history` → `undo`.

## 2. The studio and the crew

The roster is the project's studio (`agents/<id>.agent.json`), shared by every client. The
full model — board, feedback, decisions with measured effects, loops, playtests, messages,
budgets and the headless runner — is in [STUDIO](STUDIO.md). This section covers the editor.

Each agent has:

| Setting | Options |
|---|---|
| Role and focus | 24 roles across direction, production, design, engineering, art, audio, writing and QA (creative director, producer, level designer, gameplay/AI/graphics programmer, lighting/VFX/environment artist, sound designer, writer, playtester, critic…), plus a focus ("enemy AI", "onboarding") and tags, so several agents can share a role. The role's mission becomes part of its system prompt. |
| Provider and model | Anthropic (Messages API; default `claude-opus-5-5`), or any OpenAI-compatible API: OpenAI, DeepSeek, OpenRouter, Groq, or local Ollama / LM Studio / vLLM / llama.cpp. In the editor, keys live in the macOS Keychain; the headless runner reads them from the environment. |
| Autonomy | *Observe* (read-only tools), *Ask* (each mutating call waits for your Allow/Decline), *Autonomous*. |
| Permissions | Per tool category (scene, entities, world, assets, behaviors, simulation, viewport, files, rendering, network, studio): *Default* (follow autonomy), *Allow*, *Ask*, or *Off* (the tools are not even offered to the model). Categories come from each tool's `_meta["skywalker/category"]`. **Network** (`asset_download`) asks by default even for autonomous agents; **studio** tools (board, feedback, messages, playtests) are available at every autonomy level. |
| Mission and instructions | Override the role's mission; add standing instructions such as style guides, naming rules or constraints. |
| Reports to | The agent's lead. |
| Playtester persona | Policy, reaction time, skill, curiosity, patience — used by `playtest_run`. |
| Memory | Long-term notes kept with `studio_memory`. They appear in the agent's system prompt and can be edited in the designer. |
| Rounds | Max tool rounds per task (default 40). |
| Usage | Tokens, estimated cost and tool calls per agent, tracked by the studio (`studio/usage.json`). |

Open the **Agent Designer** from the slider button in a chat header, from a Studio roster
card, or in Settings → Crew. Edits save automatically to the project's agent file (never API
keys), so a team can commit and share its studio. Add agents by role or spawn a whole team
(`starter_crew`, `indie_trio`, `aaa_strike_team`, `narrative_team`, `qa_squad`, `art_team`,
`audio_team`). A new project starts with the starter crew; crews from earlier editor versions
are migrated into the project once.

How the crew works together:
- **The Studio panel.** Roster with live status, a Kanban board, feedback with the
  director's verdicts and measured effects, loop timelines and message threads.
- **Loops.** *Run with Crew* drives a studio loop (`playtest_fix_verify`, `art_pass_with_critic`,
  `balance_tuning`, `vertical_slice_sprint`, `bug_bash`, or your own) through
  `studio_loop_start` / `studio_loop_advance`: the engine runs playtests and verification,
  the crew runs agent stages — in parallel when the stage allows it. Loops replace the
  earlier pipelines.
- **Delegation.** Direction and production agents get an extra `crew_delegate` tool in chat,
  so they can hand a task to a teammate and review the report.
- **Weave.** From any behavior's intent, Weave asks the gameplay programmer to derive a
  spec, write the Wander code with a test per rule, attach it, and run `wander_test`.

You see agents in action through:
- presence avatars in the toolbar, with a pulsing dot while working;
- a toast over the viewport;
- the Activity feed, which records who did what (including studio decisions, feedback,
  playtests and loops);
- every edit being undoable.

Implementation notes (`editor/Sources/Crew`, `editor/Sources/Studio`):
- System prompts and tool permissions come from the engine (`studio_agent_brief`), so the
  editor, the headless runner and external agents run an agent identically.
- Provider histories are append-only. Anthropic thinking blocks are echoed back
  unchanged, and refusals are handled via `stop_reason` (a refused turn's tools never run).
- Server-side refusal fallback (`fallbacks: "default"`) is enabled on supported Claude
  models.
- Screenshots go back as `tool_result` images for Anthropic. OpenAI-compatible providers
  receive them as a follow-up user message, if the model supports vision.
- Each agent runs one conversation at a time.
- Tool results are never dropped: if you stop an agent mid-turn, they are delivered with
  the next message.

### External agents as studio members

An MCP client joins the roster by connecting as `<client>/<agent id>` (e.g.
`skywalker mcp --attach` with client name `claude-code/mira`, or `skywalker call … --attach
--as claude-code/mira`), or by passing `as: "<agent id>"` on studio calls — which is how
Claude Code subagents sharing one connection identify themselves. `studio_agent_brief`
returns the agent's system prompt and permitted tools for any harness, and a loop's
assignments can be executed by external subagents (see [STUDIO](STUDIO.md#identity-how-external-tools-join)).

## 3. Custom tools

When no engine tool does what an agent needs, it defines its own with `tool_define`: Wander
code (`fn run(args)`) or a composite pipeline of tool calls, saved in the project as
`tools/<name>.tool.json` and registered as `user_<name>` for every client. Connected clients
can also host tools of their own (`skywalker/tools/register` over MCP, or `tool_host_register`).
Capabilities are an explicit allowlist, calls run with the caller's permissions, limits bound
every call, edits are undoable, and tools that mutate the project wait for a human's
`tool_approve` under the default policy. See [CUSTOM_TOOLS](CUSTOM_TOOLS.md).

## 4. Generative assets

```
asset_request {kind: mesh|texture|sprite|audio|music|video, prompt, style?, target?}
        │   (queued, visible to every client)
        ▼
generator / agent produces a file in the project
        ▼
asset_complete {id, path}
  → mesh:    imported (OBJ) and assigned to the target
  → texture: set on the target's material
  → sprite:  target gets a `sprite` component with the image (camera-facing in 3D scenes)
```

Any MCP-capable generator bridge can serve the queue: poll `asset_requests`, generate,
then call `asset_complete`. Built-in provider adapters for image and 3D generation are on
the [roadmap](ROADMAP.md). Audio, music and video results are stored and listed but not
yet played back (the audio engine is also on the roadmap).
