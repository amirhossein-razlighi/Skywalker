# skywalker-agents

Build, run and observe teams of AI agents on the [Skywalker](../README.md) game engine.

- **Typed engine client** (sync and async): attach to the editor, spawn a headless engine (`skywalker serve`), or use a fake;
  typed wrappers for every engine tool; the live event stream; Python tools served to every agent as `py_*`.
- **Agents** that run any studio roster member (its brief, tools and permissions) on Claude (official Anthropic SDK, default
  `claude-opus-5-5`) or any OpenAI-compatible model, with budgets, approvals and middleware.
- **Shared memory** (agent, team, project, global; SQLite + FTS5, optional vectors), also as MCP tools for Claude Code and Codex.
- **Communication** over studio threads: direct messages, topics, request/reply, handoffs, the board as a blackboard, agent cards.
- **Workflows** in Python or YAML, patterns (director/critic, debate, plan-and-execute, map-reduce, playtest loop), engine Studio
  loops, checkpoints and resume, record/replay, evals.
- **Tracing** with OpenTelemetry GenAI conventions, JSONL run logs, cost accounting, `sky-agents trace`.
- **Plugins** via entry points for tools, providers, stores, patterns, roles and hooks; adapters for LangGraph, CrewAI, the OpenAI
  Agents SDK, Pydantic AI and the Claude Agent SDK.

```bash
uv sync                                              # Python >= 3.10
uv run python examples/playtest_triage_fix/run.py --dry-run
uv run sky-agents --help
```

Documentation: [docs/PYTHON_AGENTS.md](../docs/PYTHON_AGENTS.md).
