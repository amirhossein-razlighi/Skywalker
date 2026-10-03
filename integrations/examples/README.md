# Examples: driving Skywalker from the model SDKs

Skywalker is an MCP server, so any agent framework that speaks MCP can use every engine tool. These scripts are minimal,
dependency-light starting points. They are syntax-checked by the test-suite but **not run against real APIs in CI** (they need keys).

| Example | Framework | What it shows |
|---|---|---|
| [`openai_agents_sdk/skywalker_agents.py`](openai_agents_sdk/skywalker_agents.py) | [OpenAI Agents SDK](https://openai.github.io/openai-agents-python/mcp/) (`MCPServerStdio`) | `scene`: one agent builds and looks at an island. `studio`: roster members run as separate `Agent`s sharing the engine while the script drives a studio loop (`studio_loop_start` / `studio_loop_advance`). |
| [`anthropic_tool_runner/skywalker_claude.py`](anthropic_tool_runner/skywalker_claude.py) | [Anthropic Python SDK](https://github.com/anthropics/anthropic-sdk-python/blob/main/helpers.md) tool runner + `async_mcp_tool` | A tool-runner loop over all engine tools with the `skywalker-core` skill as the system prompt. |

Run them from this directory after `pip install openai-agents` (or `pip install "anthropic[mcp]"`) and exporting the provider key. The scripts start
`skywalker mcp --project DIR` themselves; put the `skywalker` binary on `PATH` or pass `--binary`.

Why the engine side is already agent-ready:

- 111 typed tools with LLM-oriented descriptions and JSON Schemas (`skywalker tools --json`), annotations (`readOnlyHint`, `openWorldHint`), and per-category `_meta`.
- Images come back as MCP image content (captures) so vision models can look at what they built.
- `resources/read skywalker://docs/<name>` and `prompts/get` give frameworks the docs, skills and studio-role prompts without any files.
- Deterministic simulation and `playtest_run` give agents objective feedback to loop on.
