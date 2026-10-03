# Agents in Skywalker

There are three ways an AI works with Skywalker, and all three use the same
[tool surface](TOOLS.md):

1. **External agents over MCP.** Claude Code, Codex, Cursor, Gemini CLI, or any MCP
   client.
2. **The in-editor crew ("Cloudlings").** Specialized agents with avatars, roles,
   autonomy levels and pipelines.
3. **Generators.** Image, 3D, audio, music and video models that fulfill asset requests.

## 1. MCP

Skywalker implements the [Model Context Protocol](https://modelcontextprotocol.io) server
side: `initialize` (protocol versions 2025-11-25 → 2024-11-05), `tools/list`, `tools/call`
(text, structured content, and PNG images), `ping`, plus empty `resources`/`prompts` lists.

| Mode | Command | Use when |
|---|---|---|
| Attach to the editor | `skywalker mcp --attach [socket]` | You want to watch and co-edit live. Edits appear in the activity feed as `mcp:<client>`. |
| Headless | `skywalker mcp --project DIR [--scene FILE]` | CI, batch generation, or no GUI. |

The editor's socket (`~/.skywalker/editor.sock`, mode 0600) starts with the editor. Toggle
it from the status bar or Settings → External Agents.

```bash
claude mcp add skywalker -- /path/to/skywalker mcp --attach
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
4. For behaviors: read `wander_reference` once, then `wander_check`, `behavior_set`,
   `sim_control step`, `sim_input`, `logs`, and finally `sim_control stop`.
5. If something went wrong, `history` → `undo`.

## 2. The crew

The crew lives in the editor's **Agents** dock. Each Cloudling has:

| Setting | Options |
|---|---|
| Role | Creative Director, Level Designer, Gameplay Programmer, Lighting Artist, Writer, Asset Artist, QA Tester. The role becomes part of its system prompt. |
| Provider and model | Anthropic (Messages API; default `claude-opus-5-5`), or any OpenAI-compatible API: OpenAI, DeepSeek, OpenRouter, Groq, or local Ollama / LM Studio / vLLM / llama.cpp. Keys live in the macOS Keychain. |
| Autonomy | *Observe* (read-only tools), *Ask* (each mutating call waits for your Allow/Decline), *Autonomous*. |
| Permissions | Per tool category (scene, entities, world, assets, behaviors, simulation, viewport, files, rendering): *Default* (follow autonomy), *Allow*, *Ask*, or *Off* (the tools are not even offered to the model). Categories come from each tool's `_meta["skywalker/category"]`. |
| Mission and instructions | Override the role's mission; add standing instructions such as style guides, naming rules or constraints. |
| Memory | Long-term notes the agent keeps with `memory_note` / `memory_forget`. They appear in its instructions at the start of every conversation and can be edited in the designer. |
| Rounds | Max tool rounds per message (default 40). |
| Usage | Input, cached and output tokens per agent, as reported by the provider. Shown in the chat header and designer. |

Open the **Agent Designer** from the slider button in a chat header, or in Settings → Crew.
**Save to Project** writes `agents/<name>.agent.json` (everything but provider ids and keys),
so a team can commit and share its crew. Settings → Crew lists the project's agent files to load.

How the crew works together:
- **Delegation.** The Creative Director gets an extra `crew_delegate` tool, so it can
  plan, hand tasks to teammates, and review their reports.
- **Pipelines.** Ordered stages, each a Cloudling plus an instruction. Mark a step
  *parallel* (branch icon) to run it at the same time as the previous step. Each stage
  receives the goal plus every earlier stage's report, and parallel agents are told who
  else is working.
- **Weave.** From any behavior's intent, Weave asks the gameplay programmer to write,
  check, attach and test the Wander code.

You see agents in action through:
- presence avatars in the toolbar, with a pulsing dot while working;
- a toast over the viewport;
- the Activity feed, which records who did what;
- every edit being undoable.

Implementation notes (`editor/Sources/Crew`):
- Provider histories are append-only. Anthropic thinking blocks are echoed back
  unchanged, and refusals are handled via `stop_reason`.
- Server-side refusal fallback (`fallbacks: "default"`) is enabled on supported Claude
  models.
- Screenshots go back as `tool_result` images for Anthropic. OpenAI-compatible providers
  receive them as a follow-up user message, if the model supports vision.
- Each agent runs one loop at a time.
- Tool results are never dropped: if you stop an agent mid-turn, they are delivered with
  the next message.

## 3. Generative assets

```
asset_request {kind: mesh|texture|sprite|audio|music|video, prompt, style?, target?}
        │   (queued, visible to every client)
        ▼
generator / agent produces a file in the project
        ▼
asset_complete {id, path}
  → mesh:    imported (OBJ) and assigned to the target
  → texture: set on the target's material
  → sprite:  target becomes a camera-facing textured quad
```

Any MCP-capable generator bridge can serve the queue: poll `asset_requests`, generate,
then call `asset_complete`. Built-in provider adapters for image and 3D generation are on
the [roadmap](ROADMAP.md). Audio, music and video results are stored and listed but not
yet played back (the audio engine is also on the roadmap).
