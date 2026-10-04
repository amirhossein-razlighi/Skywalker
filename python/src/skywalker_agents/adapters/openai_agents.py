"""OpenAI Agents SDK (``[openai-agents]`` extra): Skywalker tools as ``FunctionTool``s, or the whole engine as
an MCP server.

    agent = agents.Agent(name="Mira", tools=as_openai_agents_tools(my_tools))
    # or every engine tool, live:
    async with agents.mcp.MCPServerStdio(params=skywalker_mcp_params()) as sky:
        agent = agents.Agent(name="Mira", mcp_servers=[sky])
"""

from __future__ import annotations

import json
from collections.abc import Sequence
from typing import Any

from ..engine.process import default_editor_socket, find_binary
from ..tools.base import Tool, ToolContext


def as_openai_agents_tools(tools: Sequence[Tool], *, ctx: ToolContext | None = None) -> list[Any]:
    from agents import FunctionTool

    out = []
    for t in tools:

        async def invoke(_run_ctx: Any, args_json: str, _t: Tool = t) -> str:
            try:
                args = json.loads(args_json or "{}")
            except json.JSONDecodeError:
                return "ERROR: arguments were not valid JSON"
            res = await _t.run(args, ctx)
            return ("ERROR: " if res.is_error else "") + (res.text or json.dumps(res.structured or {}))

        out.append(
            FunctionTool(
                name=t.name,
                description=t.description,
                params_json_schema=t.input_schema,
                on_invoke_tool=invoke,
                strict_json_schema=False,
            )
        )
    return out


def skywalker_mcp_params(*, socket: str | None = None, binary: str | None = None) -> dict[str, Any]:
    """``MCPServerStdio`` params that attach to the running editor (or ``skywalker serve`` at ``socket``)."""
    return {"command": find_binary(binary), "args": ["mcp", "--attach", socket or default_editor_socket()]}
