# Integrations

Everything that connects other tools to Skywalker lives here.

| Directory | What it is | Generated? |
|---|---|---|
| [`skills-src/`](skills-src) | **The single source**: skills (`skills/<name>/SKILL.md` + `references/`), studio-role subagents (`agents/`), workflow commands (`commands/`), project guidance (`context.md`), plugin metadata (`meta.json`) | No: edit here |
| [`claude-code/`](claude-code) | Claude Code plugin (`.claude-plugin/plugin.json`, `skills/`, `agents/`, `commands/`, `.mcp.json`); the marketplace is `/.claude-plugin/marketplace.json` at the repo root | Yes |
| [`codex/`](codex) | `AGENTS.md`, `.agents/skills/`, `.codex/config.toml`, `.codex/agents/*.toml` | Yes |
| [`gemini/`](gemini) | Gemini CLI extension: `gemini-extension.json`, `GEMINI.md`, `skills/`, `agents/`, `commands/` | Yes |
| [`cursor/`](cursor) | `.cursor/mcp.json`, `rules/*.mdc`, `skills/`, `agents/`, `commands/` | Yes |
| [`examples/`](examples) | OpenAI Agents SDK and Anthropic tool-runner scripts driving the engine | No |
| `blender/`, `dcc/` | Blender add-on and the Python helper library of the DCC bridge (embedded in the binary) | No |

Tools: `generate.py` (emit all outputs; `--check` fails when stale), `check_skills.py` (lints the sources against `skywalker tools --json`). Both are standard-library Python and run in the test-suite.
Install with `skywalker setup <claude|codex|gemini|cursor|all>` (see [docs/INTEGRATIONS.md](../docs/INTEGRATIONS.md)).

## Sources used to verify the formats (official documentation)

Claude Code
- Plugins reference (manifest, layout, `.mcp.json`, agents, commands, hooks): https://code.claude.com/docs/en/plugins-reference
- Create a marketplace (`.claude-plugin/marketplace.json`, `source`, add/install commands): https://code.claude.com/docs/en/plugin-marketplaces
- Skills (`SKILL.md` frontmatter, directories, progressive disclosure): https://code.claude.com/docs/en/skills
- Subagents (frontmatter, MCP tool patterns): https://code.claude.com/docs/en/sub-agents
- Plugin components (agents in plugins, commands, MCP servers): https://code.claude.com/docs/en/plugins/components

OpenAI Codex
- Skills (`.agents/skills`, `SKILL.md`, `agents/openai.yaml`, `allow_implicit_invocation`): https://developers.openai.com/codex/skills
- MCP (`[mcp_servers.<name>]` in `~/.codex/config.toml` or `.codex/config.toml`, timeouts, `codex mcp add`): https://developers.openai.com/codex/mcp
- AGENTS.md discovery (`~/.codex/AGENTS.md`, root-to-cwd concatenation, `project_doc_max_bytes`): https://developers.openai.com/codex/guides/agents-md
- Subagents / custom agents (`.codex/agents/*.toml`): https://developers.openai.com/codex/subagents

Gemini CLI
- Extensions reference (`gemini-extension.json`, `GEMINI.md`, `commands/`, `skills/`, `agents/`, `${extensionPath}`): https://geminicli.com/docs/extensions/reference/
- Agent Skills: https://geminicli.com/docs/cli/skills/
- MCP servers (`mcpServers` in `settings.json`, `timeout`, `trust`): https://geminicli.com/docs/tools/mcp-server/
- Custom commands (TOML `prompt`, `description`, `{{args}}`): https://geminicli.com/docs/cli/custom-commands/
- Subagents (`.gemini/agents/*.md`): https://geminicli.com/docs/core/subagents/

Cursor
- MCP (`.cursor/mcp.json`, `~/.cursor/mcp.json`, interpolation): https://cursor.com/docs/context/mcp
- Rules (`.cursor/rules/*.mdc`, `alwaysApply`, `globs`, `AGENTS.md`): https://cursor.com/docs/context/rules
- Skills (`.cursor/skills`, `.agents/skills`): https://cursor.com/docs/context/skills
- Subagents (`.cursor/agents/*.md`): https://cursor.com/docs/context/subagents

Protocols and SDKs
- Model Context Protocol (resources, prompts): https://modelcontextprotocol.io
- OpenAI Agents SDK, MCP: https://openai.github.io/openai-agents-python/mcp/
- Anthropic Python SDK MCP helpers and tool runner: https://github.com/anthropics/anthropic-sdk-python/blob/main/helpers.md
- Poly Haven public API (referenced in the assets skill): https://polyhaven.com/our-api

Note: the OpenAI docs URLs above redirect to learn.chatgpt.com at the time of writing.
