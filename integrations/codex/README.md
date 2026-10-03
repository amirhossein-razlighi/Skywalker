# Skywalker for Codex

Mirror of the files Codex reads, **generated** from `../skills-src` by `python3 integrations/generate.py`.

| File | Goes to |
|---|---|
| `AGENTS.md` | the project root, or `~/.codex/AGENTS.md` (only the block between the SKYWALKER markers is managed) |
| `.agents/skills/` | `.agents/skills` in the project, or `~/.agents/skills` (`skywalker-*` skills; `skywalker-cmd-*` are explicit workflows: `$skywalker-cmd-new-game <pitch>`) |
| `.codex/config.toml` | merge into `~/.codex/config.toml` (or a trusted project's `.codex/config.toml`) |
| `.codex/agents/*.toml` | `.codex/agents` or `~/.codex/agents`: the studio roles as custom agents |

Easiest: `skywalker setup codex --global` (merges, backs up, idempotent) or `codex mcp add skywalker -- skywalker mcp --auto`. See [docs/INTEGRATIONS.md](../../docs/INTEGRATIONS.md).
