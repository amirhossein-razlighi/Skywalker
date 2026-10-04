"""Serve Python tools (memory, your plugins' tools) as an MCP server over stdio (``[mcp]`` extra).

``sky-agents serve-mcp --project DIR`` gives Claude Code, Codex, Cursor or Gemini CLI the same shared
memory the Python agents use::

    claude mcp add sky-memory -- sky-agents serve-mcp --project /path/to/game --agent claude-code
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any

from ..tools.base import Tool, ToolContext


def build_server(tools: Sequence[Tool], *, name: str = "sky-agents", agent: str = "", instructions: str = "") -> Any:
    """A low-level MCP ``Server`` exposing ``tools`` with their JSON Schemas."""
    import mcp.types as types
    from mcp.server.lowlevel import Server

    server: Any = Server(name, instructions=instructions or None)
    by_name = {t.name: t for t in tools}

    @server.list_tools()  # type: ignore[untyped-decorator]
    async def list_tools() -> list[types.Tool]:
        return [
            types.Tool(
                name=t.name,
                description=t.description,
                inputSchema=t.input_schema,
                annotations=types.ToolAnnotations(readOnlyHint=not t.mutates),
            )
            for t in tools
        ]

    @server.call_tool(validate_input=True)  # type: ignore[untyped-decorator]
    async def call_tool(tool_name: str, arguments: dict[str, Any]) -> types.CallToolResult:
        t = by_name.get(tool_name)
        if t is None:
            return types.CallToolResult(
                content=[types.TextContent(type="text", text=f"unknown tool {tool_name}")], isError=True
            )
        result = await t.run(arguments, ToolContext(agent_id=agent, actor=f"mcp:{agent or name}"))
        content: list[Any] = [types.TextContent(type="text", text=result.text or "(empty)")]
        for b in result.content:
            if b.type == "image" and b.data:
                content.append(types.ImageContent(type="image", data=b.data, mimeType=b.mimeType or "image/png"))
        return types.CallToolResult(content=content, structuredContent=result.structured, isError=result.is_error)

    return server


async def serve_stdio(
    tools: Sequence[Tool], *, name: str = "sky-agents", agent: str = "", instructions: str = ""
) -> None:
    """Runs the MCP server on stdin/stdout until the client disconnects."""
    from mcp.server.stdio import stdio_server

    server = build_server(tools, name=name, agent=agent, instructions=instructions)
    async with stdio_server() as (read, write):
        await server.run(read, write, server.create_initialization_options())
