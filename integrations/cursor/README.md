# Skywalker for Cursor

A ready-to-copy `.cursor/` directory, **generated** from `../skills-src` by `python3 integrations/generate.py`:

- `.cursor/mcp.json`: the MCP server (`skywalker mcp --auto`; put `skywalker` on `PATH` or use `skywalker setup cursor` for an absolute path)
- `.cursor/rules/skywalker.mdc` (always on) and `skywalker-wander.mdc` (when editing `.wander` files)
- `.cursor/skills/` (the `skywalker-*` skills), `.cursor/agents/` (studio roles, `/level-designer ...`), `.cursor/commands/` (`/new-game`, `/look-dev`, `/playtest-loop`, ...)

Install: `skywalker setup cursor` (merges `mcp.json`, backs up changed files), or copy the folder into your project (`~/.cursor` works for MCP + skills). See [docs/INTEGRATIONS.md](../../docs/INTEGRATIONS.md).
