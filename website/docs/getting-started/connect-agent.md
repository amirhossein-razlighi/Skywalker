# Connect an AI agent

Skywalker is an MCP server. One command wires Claude Code, Codex, Gemini CLI or Cursor to it: the MCP entry, thirteen
`skywalker-*` skills that teach the engine's loop and conventions, eight studio-role subagents and five workflow
commands. After that, your agent can build, look at and test games in the same project you have open.

## One command

```bash
skywalker setup claude            # or: codex | gemini | cursor | all
```

| Option | Effect |
|---|---|
| *(none)* | Project scope: writes `./.mcp.json`, `./.claude/…`, `./CLAUDE.md` (or the other tool's equivalents) in the current folder |
| `--global` | User scope: `~/.claude`, `~/.codex`, `~/.gemini`, `~/.cursor` |
| `--project DIR` | Configure another folder |
| `--dry-run` | Show every change as a diff; write nothing |
| `--print` | Print the MCP entry to paste by hand |
| `--no-skills` | Only the MCP entry |
| `--mode auto\|attach\|headless` | How the server connects (default `auto`) |
| `--binary PATH` | Path to write for the `skywalker` executable (default: the binary you run) |

Setup **merges** into existing configs (JSON keys, TOML tables and Markdown outside its own markers are kept; invalid
files are refused rather than overwritten), backs up every file it changes to `.skywalker/setup-backups/<timestamp>/`,
and is idempotent: run it again after an update.

## What the server does

The MCP entry runs `skywalker mcp --auto`:

1. If the editor is running with its agent socket on, the agent **attaches** to it. You watch its edits appear live,
   they show up in the Activity feed as `mcp:<client>`, and you can undo them.
2. Otherwise it starts a **headless** engine on the current folder. Edits live in memory until the agent calls
   `scene_save`.

| Mode | Command | Use when |
|---|---|---|
| Auto | `skywalker mcp --auto [--project DIR]` | The default for agent configs |
| Attach | `skywalker mcp --attach [SOCKET]` | You want to watch and co-edit live |
| Headless | `skywalker mcp --project DIR [--scene FILE]` | CI, batch generation, no GUI |

The editor's socket lives at `~/.skywalker/editor.sock` (mode 0600). Toggle it from the status bar ("MCP listening") or
Settings › External Agents.

## Per tool

=== "Claude Code"

    ```bash
    skywalker setup claude
    ```

    Or install the plugin from the repository's marketplace (inside Claude Code: `/plugin marketplace add`, then
    `/plugin install skywalker@skywalker`):

    ```bash
    claude plugin marketplace add /path/to/Skywalker
    claude plugin install skywalker@skywalker
    ```

    The plugin brings the skills (`/skywalker:skywalker-core`, …), the subagents (`skywalker:level-designer`, …), the
    commands (`/skywalker:new-game`, `/skywalker:look-dev`, `/skywalker:playtest-loop`, `/skywalker:studio-status`,
    `/skywalker:studio-setup`) and an MCP entry that runs `skywalker mcp --auto` (so `skywalker` must be on `PATH`).
    By hand: `claude mcp add skywalker -- /path/to/skywalker mcp --auto`.

=== "Codex"

    ```bash
    skywalker setup codex --global
    ```

    Writes `[mcp_servers.skywalker]` to `~/.codex/config.toml` with a 600 s tool timeout (captures, playtests and
    design-app jobs outlast the 60 s default), the skills to `~/.agents/skills`, the studio roles as custom agents and
    an `AGENTS.md` block. Skills apply implicitly or explicitly (`$skywalker-core`); workflows are skills named
    `skywalker-cmd-*` (`$skywalker-cmd-new-game a cozy boat game`).

=== "Gemini CLI"

    ```bash
    gemini extensions install /path/to/Skywalker/integrations/gemini
    ```

    Or `skywalker setup gemini`. The extension registers the MCP server with a ten-minute timeout, `GEMINI.md`
    context, the skills, the subagents (`@level-designer …`) and the commands (`/new-game`, `/look-dev`,
    `/playtest-loop`, `/studio-status`, `/studio-setup`).

=== "Cursor"

    ```bash
    skywalker setup cursor
    ```

    Writes `.cursor/mcp.json`, an always-on rule with the loop and conventions, a rule for `.wander` files, skills,
    subagents and commands. Restart Cursor (or toggle the server in Settings › MCP) after the first setup.

=== "Any MCP client"

    ```json
    {"mcpServers": {"skywalker": {"command": "/path/to/skywalker", "args": ["mcp", "--auto"]}}}
    ```

    SDK examples (an agent built with the OpenAI Agents SDK running a studio loop, and the Anthropic tool runner) are
    in `integrations/examples/` in the repository.

## First conversation

Open the project in your agent and ask for something concrete:

> Look at the scene, then add a lighthouse on the rocky point with a rotating beam, and show me a capture from the
> pier.

A well-set-up agent will orient itself, act in batches, and look at its own work:

```tool
engine_info {}
scene_overview {}
batch {"operations": [{"tool": "entity_create", "args": {"name": "Lighthouse", "mesh": "cylinder", "position": [12, 3, -6], "scale": [1.6, 6, 1.6], "color": "#f2efe6"}}]}
viewport_capture {"annotate": true, "eye": [0, 3, 10], "target": [12, 4, -6]}
```

## Troubleshooting

| Symptom | Fix |
|---|---|
| Tools missing in the client | Run `skywalker setup <tool>`; check the server list (`/mcp` in Claude Code, `codex mcp list`, `gemini mcp list`); Claude Code asks you to approve project `.mcp.json` servers once |
| "no editor is running" in the server log | Expected in `--auto` mode: it fell back to headless. Start the editor and enable External Agents to co-edit live |
| Long calls time out | Raise the client's tool timeout (Codex `tool_timeout_sec`, Gemini `timeout`); setup already sets 600 s |
| Headless edits vanish | Headless state is in memory: ask the agent to `scene_save` |
| `asset_download` or `dcc_*` calls wait | They need your approval by design: they reach the network or run other programs |
| Two copies of the skills in Claude Code | You installed both the plugin and `setup claude`; re-run setup with `--no-skills` |

!!! agent "For agents"

    Read the [Agent Guide](../agents/index.md): how to see the scene, the MCP resources and prompts, permissions, and
    the patterns that make agents effective here.

Next: the [Manual](../manual/index.md) or the [Agent Guide](../agents/index.md).
