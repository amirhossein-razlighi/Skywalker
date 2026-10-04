# Permissions and approvals

Agents in Skywalker can do everything a person can, which makes it important to control *when* they do it. Every tool
declares what kind of action it is; MCP clients use those declarations to ask for approval, and the in-editor crew and
the studio runner apply per-agent autonomy and per-category permissions on top.

## Tool annotations

Every tool carries MCP annotations and a category:

| Annotation | Meaning | Count |
|---|---|---|
| `readOnlyHint: true` | Never changes the project (queries, captures, references) | 63 of 168 |
| `readOnlyHint: false` | Changes the project; every change is undoable and attributed | 105 |
| `destructiveHint: true` | May remove or replace data: `entity_delete`, `behavior_remove`, `scene_load`, `scene_new`, `studio_agent_remove` | 5 |
| `openWorldHint: true` | Reaches outside the engine: the network or other programs | 14 |
| `_meta["skywalker/category"]` | `scene`, `entity`, `history`, `view`, `render`, `world`, `asset`, `wander`, `code`, `sim`, `physics`, `animation`, `ui`, `dialogue`, `studio`, `dcc`, `files`, `network` | |

The open-world tools are `asset_download` (network), every design-app tool that runs a program (`dcc_run_script`,
`dcc_convert`, `dcc_export`, `dcc_edit_asset`, `dcc_generate`, `dcc_install_addon`, the `dcc_session_*` tools) and the
two that start the game outside the engine (`game_build`, `game_run`). MCP clients such as Claude Code, Codex, Gemini
CLI and Cursor show them as actions that need your approval.

The reference marks every tool with these badges: see [Tools](../reference/tools/index.md).

## The crew: autonomy and categories

Each studio agent has an **autonomy** level and optional **per-category permissions**. The engine resolves them the
same way for the editor's crew, the headless runner and `studio_agent_brief` (which tells external harnesses which
tools a roster member may use):

| Autonomy | Read-only tools | Mutating tools | Open-world and network tools |
|---|---|---|---|
| **Observe** | allowed | off (not offered to the model) | off |
| **Ask** | allowed | each call waits for your Allow or Decline | ask |
| **Autonomous** | allowed | allowed | **ask** (unless the category is explicitly allowed) |

Per-category settings override the autonomy for that category: **Default** (follow autonomy), **Allow**, **Ask** or
**Off**. *Off* removes the tools from what the model is offered at all. Two rules always apply:

- **Studio coordination tools** (board, feedback, messages, playtests: category `studio`) are available at every
  autonomy level unless explicitly turned off: they never change the game.
- **Reaching outside the project** (open-world tools and the `network` category) asks even for autonomous agents,
  unless you explicitly allow that category for that agent.

```json
{"id": "nimbus", "role": "environment_artist", "autonomy": "autonomous",
 "permissions": {"network": "allow", "dcc": "ask", "code": "off"}}
```

Set them in the editor's Agent Designer, or with a tool:

```tool
studio_agent_define {"id": "nimbus", "autonomy": "autonomous", "permissions": {"network": "allow", "dcc": "ask", "code": "off"}}
```

## Headless runs

`skywalker studio run` has no human to ask. Tools that resolve to *ask* are declined unless you pass `--yes`, and
inside loops the loop and roster control tools are withheld from agents. Keys come from the environment only and are
never written to disk.

```bash
export ANTHROPIC_API_KEY=...
skywalker studio run --project ~/Games/coin_run --loop playtest_fix_verify --iterations 3
```

## Native code is trusted code

Native C++ modules and ahead-of-time compiled behaviors run inside the engine process with your user's permissions,
like any program built on your machine. The tools that build and load them (`native_build`, `wander_compile_native`)
are mutating tools in the `code` category: give agents `code: "ask"` or `"off"` if you do not want them compiling
C++, and review agent-written C++ before building it. Wander itself stays sandboxed: no file, network or clock access,
a step budget per handler, and seeded randomness.

## Design apps and downloads

- `asset_download` accepts only licenses that allow the project's use (CC0, CC-BY with attribution, MIT, public
  domain, or terms you accepted); unknown or all-rights-reserved licenses are refused and non-commercial licenses
  produce warnings. Every download records its license, author and source and appends to `CREDITS.md`.
- Design-app scripts run with an argument vector (never a shell), write only inside the project, open `.blend` files
  with auto-run scripts disabled, have a timeout and can be cancelled. The live Blender bridge only accepts requests
  carrying a random token readable by your user.

!!! agent "For agents"

    Ask for approval once, up front, before a run of open-world calls ("I need to download three CC0 models from
    Poly Haven and convert them with Blender"), instead of retrying a declined call in a loop. Check what you are
    allowed to do as a roster member:

    ```tool
    studio_agent_brief {"agent": "nimbus", "loop_member": true}
    ```
