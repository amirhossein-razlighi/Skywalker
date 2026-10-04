"""CrewAI (``[crewai]`` extra): Skywalker tools as CrewAI ``BaseTool``s.

CrewAI calls tools synchronously, so pass a synchronous :class:`~skywalker_agents.Engine` (its event loop
runs the async tools)::

    engine = Engine.attach()
    tools = as_crewai_tools(engine.run(engine_tools(engine.aio, ["scene_overview"], agent="mira")), engine)
"""

from __future__ import annotations

import json
from collections.abc import Sequence
from typing import TYPE_CHECKING, Any

from pydantic import create_model

from ..tools.base import Tool, ToolContext

if TYPE_CHECKING:  # pragma: no cover
    from ..engine.client import Engine


def _args_model(t: Tool) -> Any:
    props = t.input_schema.get("properties") or {}
    required = set(t.input_schema.get("required") or [])
    fields: dict[str, Any] = {k: (Any, ... if k in required else None) for k in props}
    return create_model(f"{t.name}_args", **fields)


def as_crewai_tools(tools: Sequence[Tool], engine: Engine, *, ctx: ToolContext | None = None) -> list[Any]:
    from crewai.tools import BaseTool

    out = []
    for t in tools:

        def run(self: Any, _t: Tool = t, **kwargs: Any) -> str:
            args = {k: v for k, v in kwargs.items() if v is not None}
            res = engine.run(_t.run(args, ctx))
            return ("ERROR: " if res.is_error else "") + (res.text or json.dumps(res.structured or {}))

        cls = type(
            f"Sky_{t.name}",
            (BaseTool,),
            {
                "__annotations__": {"name": str, "description": str, "args_schema": type},
                "name": t.name,
                "description": t.description,
                "args_schema": _args_model(t),
                "_run": run,
            },
        )
        out.append(cls())
    return out
