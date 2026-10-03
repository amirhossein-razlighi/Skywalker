# Skywalker for Gemini CLI

A Gemini CLI extension: `gemini-extension.json` (MCP server `skywalker mcp --auto`, 10 minute timeout), `GEMINI.md` context, `skills/`, `agents/` (studio roles, `@level-designer ...`)
and `commands/` (`/new-game`, `/look-dev`, `/playtest-loop`, `/studio-status`, `/studio-setup`). **Generated** from `../skills-src` by `python3 integrations/generate.py`.

```bash
gemini extensions install /path/to/Skywalker/integrations/gemini     # or: gemini extensions link <path>
```

`skywalker` must be on `PATH`; otherwise run `skywalker setup gemini` (absolute path in `.gemini/settings.json`). See [docs/INTEGRATIONS.md](../../docs/INTEGRATIONS.md).
