"""Pydantic AI (``[pydantic-ai]`` extra): Skywalker tools as ``pydantic_ai.Tool`` (JSON-schema tools).

agent = pydantic_ai.Agent("anthropic:claude-opus-5-5", tools=as_pydantic_ai_tools(my_tools))
"""

from __future__ import annotations

import json
from collections.abc import Sequence
from typing import Any

from ..tools.base import Tool, ToolContext


def as_pydantic_ai_tools(tools: Sequence[Tool], *, ctx: ToolContext | None = None) -> list[Any]:
    from pydantic_ai import Tool as PaiTool

    out = []
    for t in tools:

        async def fn(_t: Tool = t, **kwargs: Any) -> str:
            res = await _t.run(kwargs, ctx)
            return ("ERROR: " if res.is_error else "") + (res.text or json.dumps(res.structured or {}))

        out.append(PaiTool.from_schema(fn, name=t.name, description=t.description, json_schema=t.input_schema))
    return out
