# Integrations: Claude Code, Codex, Gemini CLI, Cursor

Skywalker is an MCP server, so any MCP client can drive it. This page is about making that **native and efficient**: the agent should know the engine's
loop, conventions and pitfalls from the first message, and a team of agents should be able to work in one studio. Everything below is generated from one
source and installed with one command.

```
integrations/skills-src/   the single source: 12 skills, 8 studio-role subagents, 5 workflows, project guidance
        │  python3 integrations/generate.py
        ▼
integrations/claude-code/  Claude Code plugin  (+ .claude-plugin/marketplace.json at the repo root)
integrations/codex/        AGENTS.md, .agents/skills, .codex/{config.toml,agents}
integrations/gemini/       Gemini CLI extension (gemini-extension.json, GEMINI.md, skills, agents, commands)
integrations/cursor/       .cursor/{mcp.json,rules,skills,agents,commands}
```

All four tools implement the same [Agent Skills](https://code.claude.com/docs/en/skills) format (`SKILL.md` with `name` + `description` frontmatter, progressive
disclosure, optional `references/`), so the skills are byte-identical everywhere. Only the wrappers differ: manifests, subagent files, commands and MCP config.

## Quickstart

First build the CLI (`cmake --preset headless && cmake --build --preset headless`) or use the one inside the app, and put `skywalker` somewhere stable.
The MCP entry always runs `skywalker mcp --auto`: it **attaches to the running editor** (you watch and co-edit live, edits show up as `mcp:<client>`) and
**falls back to a headless engine** on the current directory when no editor is running.

### One command for any tool

```bash
skywalker setup claude            # or codex | gemini | cursor | all      (project scope: ./.mcp.json, ./.claude/..., ./CLAUDE.md, ...)
skywalker setup codex --global    # user scope (~/.codex/config.toml, ~/.agents/skills, ...)
skywalker setup all --dry-run     # show exactly what would change, with diffs; writes nothing
skywalker setup cursor --print    # print the MCP entry to paste by hand
```

`setup` writes the absolute path of this binary, **merges** into existing configs (JSON keys, TOML tables, and markdown outside its own markers are kept; invalid files are
refused rather than overwritten), backs up every file it changes to `<root>/.skywalker/setup-backups/<timestamp>/`, and is idempotent. Options: `--project DIR`,
`--global`, `--no-skills` (MCP entry only), `--mode auto|attach|headless`, `--binary PATH`, `--verbose`. For `claude --global` it runs `claude mcp add --scope user ...`
because Claude Code owns `~/.claude.json`.

### Claude Code (plugin)

```bash
claude plugin marketplace add /path/to/Skywalker        # the repo root has .claude-plugin/marketplace.json
claude plugin install skywalker@skywalker
# inside a session: /plugin marketplace add /path/to/Skywalker, then /plugin install skywalker@skywalker
```

The plugin brings the skills (`/skywalker:skywalker-core`, ...), the eight subagents (`skywalker:level-designer`, ...), the commands
(`/skywalker:new-game`, `/skywalker:look-dev`, `/skywalker:playtest-loop`, `/skywalker:studio-status`, `/skywalker:studio-setup`) and an `.mcp.json` that runs `skywalker mcp --auto`
(so `skywalker` must be on `PATH`; otherwise use `skywalker setup claude`, which writes an absolute path, and pass `--no-skills` if you also installed the plugin).
The server appears in `/mcp` as `plugin:skywalker:skywalker`. Validate the package with `claude plugin validate integrations/claude-code`.

### Codex (CLI, IDE, app)

```bash
skywalker setup codex --global      # ~/.codex/config.toml [mcp_servers.skywalker], ~/.agents/skills, ~/.codex/agents, ~/.codex/AGENTS.md block
# or by hand:  codex mcp add skywalker -- skywalker mcp --auto
```

Skills live in `.agents/skills` (repository) or `~/.agents/skills` and are used implicitly from their descriptions or explicitly (`$skywalker-core`). Workflows are skills named
`skywalker-cmd-*` with implicit invocation off (`$skywalker-cmd-new-game a cozy boat game`). Studio roles are custom agents in `.codex/agents/*.toml`. The config sets
`tool_timeout_sec = 600` because captures, playtests and DCC jobs outlast the 60 s default. Project-scoped `.codex/config.toml` is only read for trusted projects.

### Gemini CLI (extension)

```bash
gemini extensions install /path/to/Skywalker/integrations/gemini        # or: gemini extensions link <path> while developing
# or: skywalker setup gemini     (writes .gemini/settings.json, .gemini/skills, agents, commands, GEMINI.md block)
```

The extension manifest registers the MCP server (`skywalker mcp --auto`, 10-minute timeout), `GEMINI.md` context, the skills, the subagents (`@level-designer ...`) and
the commands (`/new-game`, `/look-dev`, `/playtest-loop`, `/studio-status`, `/studio-setup`).

### Cursor

```bash
skywalker setup cursor          # .cursor/mcp.json, rules, skills, agents, commands in this project
# or copy integrations/cursor/.cursor into your project (or ~/.cursor for MCP + skills only)
```

An always-on rule (`.cursor/rules/skywalker.mdc`) carries the loop and conventions; a `.wander` rule helps when editing behavior files; skills, subagents (`/level-designer ...`)
and commands (`/new-game`) use Cursor's native formats. Restart Cursor (or toggle the server in Settings > MCP) after the first setup.

### Any other MCP client or SDK

`{"mcpServers": {"skywalker": {"command": "/path/to/skywalker", "args": ["mcp", "--auto"]}}}`. SDK examples (OpenAI Agents SDK with a studio loop, Anthropic tool runner) are in
[`integrations/examples`](../integrations/examples/README.md).

## What the skills cover

| Skill | Covers |
|---|---|
| `skywalker-core` | The agent loop (orient, act, look, verify), conventions, batching, undo, efficiency rules, the tool index by task |
| `skywalker-world-building` | Terrain presets, sculpt/paint/layers, foliage, water, placing on ground (raycast, `place_on_surface`, `scatter`) |
| `skywalker-look-dev` | Lighting recipes (golden hour, overcast, night neon, interior), GI/SSR/clouds/god rays/auto exposure/looks/LUTs, lens (DOF, motion blur), judging renders with `samples` and `debug_view`, common mistakes |
| `skywalker-assets` | Reuse, procedural generation, licensed downloads (Poly Haven, Kenney, ...), import settings, credits |
| `skywalker-dcc` | Blender bridge: procedural models, conversion, decimation/baking, live sessions |
| `skywalker-studio` | Roster, board, feedback, director decisions with measured effects, playtest bots, loops, how external agents join as roster members |
| `skywalker-audio` | Generating and measuring SFX/music, spatial audio and the mixer, input action maps |
| `skywalker-wander` | Behavior workflow (reference, check, attach, step, trace). **Stub:** syntax comes from the live reference until Wander 2 lands |
| `skywalker-physics`, `skywalker-2d-ui`, `skywalker-animation` | **Stubs** pointing at `docs/PHYSICS.md`, `docs/2D_AND_UI.md`, `docs/ANIMATION.md` |
| `skywalker-vfx` | Effects (fire, smoke, explosions, weather, water) now; hair and GPU particles are a stub for `docs/HAIR_AND_VFX.md` |

Every tool name, argument and enum value mentioned in the skills is checked against the real tool schemas (see Maintaining).

## Multi-agent roles across tools

The studio lives in the project (`agents/*.agent.json`, `studio/*`), so every tool shares it. A **roster member** is an id; any client acts as that member by passing
`as:"<id>"` on studio calls (or connecting as `<client>/<id>`). The eight shipped subagents use the role name as the studio id:

| Studio id | Claude Code | Codex custom agent | Gemini CLI | Cursor |
|---|---|---|---|---|
| `creative_director` | `skywalker:creative-director` | `creative_director` | `@creative-director` | `/creative-director` |
| `level_designer` | `skywalker:level-designer` | `level_designer` | `@level-designer` | `/level-designer` |
| `environment_artist` (environment + lighting) | `skywalker:environment-artist` | `environment_artist` | `@environment-artist` | `/environment-artist` |
| `gameplay_programmer` | `skywalker:gameplay-programmer` | `gameplay_programmer` | `@gameplay-programmer` | `/gameplay-programmer` |
| `technical_artist` | `skywalker:technical-artist` | `technical_artist` | `@technical-artist` | `/technical-artist` |
| `sound_designer` | `skywalker:sound-designer` | `sound_designer` | `@sound-designer` | `/sound-designer` |
| `playtester` | `skywalker:playtester` | `playtester` | `@playtester` | `/playtester` |
| `critic` | `skywalker:critic` | `critic` | `@critic` | `/critic` |

Each subagent first calls `studio_agent_brief {agent, loop_member:true}` (the engine's own mission, persona, team and permitted tools for that member), claims or receives a task, identifies itself
with `as`, verifies with captures/traces/playtests, and finishes with a short report. Create the members with the `studio-setup` command (or `studio_team_template`); if the project already has a roster
(Nimbus, Aurora, ...) use those ids with the matching subagent prompt.

Running a loop from any of them: `studio_loop_start` returns assignments; start one subagent per assignment (concurrently when `parallel`), collect their reports, call `studio_loop_advance`.
The `playtest-loop` command does exactly this. Unattended: `skywalker studio run --project DIR --loop NAME` uses the headless runner with its own API keys instead.

## MCP resources and prompts

Besides tools, the server exposes (no files needed; all embedded in the binary):

| Kind | Examples |
|---|---|
| Resources | `skywalker://docs/<NAME>` (every `docs/*.md`), `skywalker://skills/<skill>` (+ `.../references/<file>`), `skywalker://tools` (live catalogue) |
| Live resources | `skywalker://studio/{overview,roster,board,feedback,loops}` (JSON), `skywalker://scene/overview` |
| Prompts | `role_<role>` (the eight studio roles), `new_game`, `look_dev`, `playtest_loop`, `studio_status`, `studio_setup`, `studio_agent {agent}` (a roster member's live brief) |

Clients that surface MCP prompts (for example as slash commands) get the workflows even without the plugin files.

## Maintaining the integrations

- **Edit only `integrations/skills-src/`** (`skills/<name>/SKILL.md` + `references/`, `agents/*.md` + `_protocol.md`, `commands/*.md`, `context.md`, `meta.json`), then run `python3 integrations/generate.py`.
  `--check` fails if the committed outputs are stale. Skills are copied verbatim; subagents, commands and manifests are rendered per tool.
- `python3 integrations/check_skills.py --tools <(skywalker tools --json)` lints the sources: every `backticked` tool-like name must exist, and every example call (`tool_name {args}`) must parse and use
  real arguments, enum values and types (including inside `batch`).
- The test-suite (`tests/test_integrations.cpp`) runs both checks plus the MCP resources/prompts and `setup` merge tests, so a tool rename that breaks a skill fails CI.
- Stubs carry `<!-- TODO(lead) -->` markers naming the doc to port: Wander 2 (`docs/WANDER.md`), physics (`docs/PHYSICS.md`), 2D/UI (`docs/2D_AND_UI.md`), animation (`docs/ANIMATION.md`), hair/VFX (`docs/HAIR_AND_VFX.md`).

## Troubleshooting

| Symptom | Fix |
|---|---|
| Tools missing in the client | Run `skywalker setup <tool>`; check the server list (`/mcp` in Claude Code, `codex mcp list`, `gemini mcp list`); Claude Code asks you to approve project `.mcp.json` servers once |
| "no editor is running" in the server log | Expected in `--auto` mode: it fell back to headless. Start the editor (and enable External Agents) to co-edit live |
| Long calls time out | Raise the client's tool timeout (Codex `tool_timeout_sec`, Gemini `timeout`); `setup` already sets 600 s |
| Headless edits vanish | Headless state is in memory: ask the agent to `scene_save` |
| `asset_download` / `dcc_*` blocked | They need the human's approval by design |
| Two copies of the skills in Claude Code | You installed both the plugin and `setup claude`; re-run setup with `--no-skills` |

## Sources

Formats were verified against the official documentation: Claude Code [plugins reference](https://code.claude.com/docs/en/plugins-reference),
[marketplaces](https://code.claude.com/docs/en/plugin-marketplaces), [skills](https://code.claude.com/docs/en/skills), [subagents](https://code.claude.com/docs/en/sub-agents);
Codex [skills](https://developers.openai.com/codex/skills), [MCP](https://developers.openai.com/codex/mcp), [AGENTS.md](https://developers.openai.com/codex/guides/agents-md);
Gemini CLI [extensions](https://geminicli.com/docs/extensions/reference/), [skills](https://geminicli.com/docs/cli/skills/), [MCP servers](https://geminicli.com/docs/tools/mcp-server/),
[custom commands](https://geminicli.com/docs/cli/custom-commands/), [subagents](https://geminicli.com/docs/core/subagents/);
Cursor [MCP](https://cursor.com/docs/context/mcp), [rules](https://cursor.com/docs/context/rules), [skills](https://cursor.com/docs/context/skills), [subagents](https://cursor.com/docs/context/subagents);
SDKs: [OpenAI Agents SDK MCP](https://openai.github.io/openai-agents-python/mcp/), [Anthropic SDK MCP helpers](https://github.com/anthropics/anthropic-sdk-python/blob/main/helpers.md).
