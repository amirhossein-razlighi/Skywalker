# Skywalker for Claude Code

A Claude Code plugin: 12 skills, 8 studio-role subagents, 5 workflow commands and the MCP server entry (`skywalker mcp --auto`). **Generated** from
`../skills-src` by `python3 integrations/generate.py`; do not edit the files here.

```bash
claude plugin marketplace add /path/to/Skywalker      # repo root: .claude-plugin/marketplace.json (this folder also has one)
claude plugin install skywalker@skywalker
claude plugin validate integrations/claude-code
```

`skywalker` must be on `PATH` (or use `skywalker setup claude`, which writes the absolute path into `.mcp.json`). Without installing the plugin you can try it with
`claude --plugin-dir integrations/claude-code`. Commands: `/skywalker:new-game`, `/skywalker:look-dev`, `/skywalker:playtest-loop`, `/skywalker:studio-status`,
`/skywalker:studio-setup`. See [docs/INTEGRATIONS.md](../../docs/INTEGRATIONS.md).
