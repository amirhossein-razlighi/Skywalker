"""Claude Agent SDK (``[claude-agent-sdk]`` extra): Skywalker inside Claude Code-style agents.

options = ClaudeAgentOptions(mcp_servers={
    "skywalker": skywalker_mcp_config(),                       # every engine tool, attached to the editor
    "memory": as_sdk_mcp_server(Memory.open(project).tools()),  # shared memory, in process
})
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any

from ..engine.process import default_editor_socket, find_binary
from ..tools.base import Tool, ToolContext


def skywalker_mcp_config(*, socket: str | None = None, binary: str | None = None) -> dict[str, Any]:
    """A stdio MCP server entry that attaches to the running editor (or ``skywalker serve`` at ``socket``)."""
    return {
        "type": "stdio",
        "command": find_binary(binary),
        "args": ["mcp", "--attach", socket or default_editor_socket()],
    }


def as_sdk_mcp_server(tools: Sequence[Tool], *, name: str = "sky-agents", ctx: ToolContext | None = None) -> Any:
    """An in-process SDK MCP server exposing ``tools``."""
    from claude_agent_sdk import create_sdk_mcp_server, tool

    sdk_tools = []
    for t in tools:

        async def handler(args: dict[str, Any], _t: Tool = t) -> dict[str, Any]:
            res = await _t.run(args, ctx)
            content: list[dict[str, Any]] = [{"type": "text", "text": res.text or "(empty)"}]
            for b in res.content:
                if b.type == "image" and b.data:
                    content.append({"type": "image", "data": b.data, "mimeType": b.mimeType or "image/png"})
            return {"content": content, "is_error": res.is_error}

        sdk_tools.append(tool(t.name, t.description, t.input_schema)(handler))
    return create_sdk_mcp_server(name=name, tools=sdk_tools)
