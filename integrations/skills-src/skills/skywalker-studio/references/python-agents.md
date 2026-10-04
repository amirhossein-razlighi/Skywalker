# The Python agent layer (skywalker-agents)

The full guide is `docs/PYTHON_AGENTS.md` (MCP resource `skywalker://docs/PYTHON_AGENTS`). This page is what a coding agent needs to use it.

## When to reach for it

- Unattended or repeated multi-agent work (overnight playtest loops, evals in CI) that should not occupy your session.
- Shared memory between your session, other sessions and studio agents.
- Workflows with checkpoints, budgets and human approvals you can resume after a crash.

## Setup

```bash
cd python && uv sync                    # or: pip install -e "python[anthropic]"
sky-agents doctor                       # binary, editor socket, keys (presence only), extras
```

## Shared memory for this session

```bash
claude mcp add sky-memory -- sky-agents serve-mcp --project /path/to/game --agent claude-code
```

Tools: `memory_remember`, `memory_recall`, `memory_forget`, `memory_summarize` (scopes agent, team, project, global; kinds fact, note, episodic,
artifact). Or serve them to every agent on the running engine: `sky-agents host-tools --project DIR --attach` (they appear as `py_memory_*`).

## Run a workflow

```bash
sky-agents run python/examples/playtest_triage_fix/workflow.yaml --attach            # on the editor's project, watch it live
sky-agents run my.loop.json --spawn --project DIR                                    # an engine loop definition, driven by Python agents
sky-agents run wf.yaml --script replies.json --fake                                  # scripted models, fake engine: no keys, no engine
sky-agents trace show latest --project DIR -v                                        # what happened, with tokens and cost
```

Approvals default to the editor when attached: the run posts in `#approvals`; answer `approve A-1f2e` or `deny A-1f2e <why>` there.

## Python in three lines

```python
engine = await AsyncEngine.auto("examples/sky_dash")                        # attach to the editor, else skywalker serve
mira = Agent("level_designer", engine=engine, memory=Memory.open("examples/sky_dash"))
result = await mira.run("Widen the lava bridge to 3 m and verify with a capture")
```

Attribution: each agent's engine calls come from `mcp:sky-agents/<agent id>`; presence, usage and messages show up in the Studio panel.
