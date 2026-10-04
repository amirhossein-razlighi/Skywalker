"""LangChain / LangGraph: Skywalker tools as ``StructuredTool`` (``[langchain]`` / ``[langgraph]`` extras).

tools = as_langchain_tools(await engine_tools(engine, ["scene_overview", "entity_create"], agent="mira"))
graph = langgraph.prebuilt.create_react_agent(model, tools)
"""

from __future__ import annotations

import json
from collections.abc import Sequence
from typing import Any

from ..tools.base import Tool, ToolContext


def as_langchain_tools(tools: Sequence[Tool], *, ctx: ToolContext | None = None) -> list[Any]:
    """Async ``StructuredTool``s (use ``ainvoke`` / async graphs). Images are summarized in the text."""
    from langchain_core.tools import StructuredTool

    out = []
    for t in tools:

        async def run(_t: Tool = t, **kwargs: Any) -> str:
            res = await _t.run(kwargs, ctx)
            text = res.text
            if res.images:
                text += f"\n[{len(res.images)} image(s) returned]"
            if res.is_error:
                return "ERROR: " + text
            if not text and res.structured is not None:
                return json.dumps(res.structured)
            return text

        out.append(
            StructuredTool.from_function(
                coroutine=run, name=t.name, description=t.description, args_schema=t.input_schema, infer_schema=False
            )
        )
    return out
