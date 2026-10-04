"""``sky-agents new plugin NAME``: scaffolds an installable plugin package."""

from __future__ import annotations

import re
from pathlib import Path

_PYPROJECT = """[build-system]
requires = ["hatchling>=1.24"]
build-backend = "hatchling.build"

[project]
name = "{dist}"
version = "0.1.0"
description = "A skywalker-agents plugin: {name}"
requires-python = ">=3.10"
dependencies = ["skywalker-agents"]

# This line is the whole integration: skywalker-agents calls register(registry) at startup.
[project.entry-points."skywalker_agents.plugins"]
{module} = "{module}:register"

[tool.hatch.build.targets.wheel]
packages = ["src/{module}"]
"""

_INIT = '''"""{name}: a skywalker-agents plugin.

Install it next to skywalker-agents (``pip install -e .``); ``sky-agents plugins`` lists what it adds.
"""

from __future__ import annotations

from typing import Any

from skywalker_agents.hooks import Middleware, ToolInvocation, ToolNext
from skywalker_agents.engine.results import ToolResult
from skywalker_agents.tools import ToolContext, tool


@tool
async def {module}_hazard_count(radius: float = 20.0, *, ctx: ToolContext) -> dict[str, Any]:
    """Count entities tagged "hazard" in the scene (an example tool: replace with your own).

    Args:
        radius: Only count hazards within this many meters of the origin.
    """
    if ctx.session is None:
        return {{"error": "needs an engine"}}
    scene = await ctx.session.call("scene_query", {{"tag": "hazard", "near": [0, 0, 0], "radius": radius}})
    entities = scene.data.get("entities", [])
    return {{"hazards": len(entities), "radius": radius}}


class AuditLog(Middleware):
    """An example hook: prints every tool call an agent makes."""

    async def on_tool(self, inv: ToolInvocation, call_next: ToolNext) -> ToolResult:
        result = await call_next(inv)
        print(f"[audit] {{inv.agent_id}} {{inv.tool.name}} -> {{'error' if result.is_error else 'ok'}}")
        return result


async def two_pass(ctx: Any, *, agent: str, task: str) -> dict[str, Any]:
    """An example workflow pattern: do the task, then review your own work once."""
    first = await ctx.agent(agent).run(task)
    second = await ctx.agent(agent).run(f"Review and improve your work on: {{task}}\\nYour report: {{first.text}}")
    return {{"first": first.text, "final": second.text}}


def register(registry: Any) -> None:
    """Called once by skywalker-agents. Add tools, providers, memory stores, patterns, roles, hooks..."""
    registry.tools.add("{module}_hazard_count", {module}_hazard_count, origin="{dist}")
    registry.hooks.add("{module}_audit", AuditLog, origin="{dist}")
    registry.patterns.add("{module}_two_pass", two_pass, origin="{dist}")
'''

_TEST = """from {module} import register


class FakeRegistry:
    def __init__(self):
        from skywalker_agents.plugins.registry import PluginRegistry

        self.inner = PluginRegistry()

    def __getattr__(self, name):
        return getattr(self.inner, name)


def test_register_adds_everything():
    reg = FakeRegistry()
    register(reg)
    assert "{module}_hazard_count" in reg.tools
    assert "{module}_audit" in reg.hooks
    assert "{module}_two_pass" in reg.patterns
"""

_README = """# {dist}

A [skywalker-agents](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/PYTHON_AGENTS.md) plugin.

```bash
pip install -e .          # next to skywalker-agents
sky-agents plugins        # shows {module}_hazard_count, {module}_audit, {module}_two_pass
pytest
```

Use the pattern from a workflow spec: `{{id: review, pattern: {module}_two_pass, args: {{agent: mira, task: "..."}}}}`.
"""


def scaffold_plugin(name: str, directory: str | Path = ".") -> Path:
    module = re.sub(r"\W+", "_", name.strip().lower()).strip("_")
    if not module or module[0].isdigit():
        raise ValueError(f"'{name}' does not make a valid Python package name")
    dist = "sky-agents-" + module.replace("_", "-")
    root = Path(directory) / dist
    if root.exists():
        raise FileExistsError(f"{root} already exists")
    fmt = {"name": name, "module": module, "dist": dist}
    (root / "src" / module).mkdir(parents=True)
    (root / "tests").mkdir()
    (root / "pyproject.toml").write_text(_PYPROJECT.format(**fmt))
    (root / "src" / module / "__init__.py").write_text(_INIT.format(**fmt))
    (root / "tests" / f"test_{module}.py").write_text(_TEST.format(**fmt))
    (root / "README.md").write_text(_README.format(**fmt))
    return root
