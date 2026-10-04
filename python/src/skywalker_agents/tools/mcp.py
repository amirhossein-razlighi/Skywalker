"""Tools from any MCP server (``[mcp]`` extra): give an agent a filesystem, a browser, a DCC app...

async with MCPToolset.stdio("npx", ["-y", "@modelcontextprotocol/server-filesystem", "."]) as fs:
    agent = Agent("mira", engine=engine, tools=fs.tools)
"""

from __future__ import annotations

import contextlib
from typing import Any

from ..engine.results import ContentBlock, ToolResult
from .base import Tool, ToolContext


class MCPToolset:
    """A live MCP client session whose tools are wrapped as :class:`Tool` (optionally name-prefixed)."""

    def __init__(self, session: Any, stack: contextlib.AsyncExitStack, *, prefix: str = "") -> None:
        self.session = session
        self._stack = stack
        self.prefix = prefix
        self.tools: list[Tool] = []

    @classmethod
    async def stdio(
        cls, command: str, args: list[str] | None = None, *, env: dict[str, str] | None = None, prefix: str = ""
    ) -> MCPToolset:
        from mcp import ClientSession, StdioServerParameters
        from mcp.client.stdio import stdio_client

        stack = contextlib.AsyncExitStack()
        read, write = await stack.enter_async_context(
            stdio_client(StdioServerParameters(command=command, args=args or [], env=env))
        )
        session = await stack.enter_async_context(ClientSession(read, write))
        await session.initialize()
        out = cls(session, stack, prefix=prefix)
        await out.refresh()
        return out

    async def refresh(self) -> list[Tool]:
        listed = await self.session.list_tools()
        self.tools = [self._wrap(t) for t in listed.tools]
        return self.tools

    def _wrap(self, spec: Any) -> Tool:
        remote = str(spec.name)
        session = self.session

        async def handler(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
            res = await session.call_tool(remote, args)
            blocks = []
            for c in res.content:
                if getattr(c, "type", "") == "text":
                    blocks.append(ContentBlock(type="text", text=c.text))
                elif getattr(c, "type", "") == "image":
                    blocks.append(ContentBlock(type="image", data=c.data, mimeType=c.mimeType))
            return ToolResult(
                tool=remote,
                content=blocks,
                structuredContent=getattr(res, "structuredContent", None),
                isError=bool(res.isError),
            )

        annotations = getattr(spec, "annotations", None)
        read_only = bool(getattr(annotations, "readOnlyHint", False)) if annotations is not None else False
        return Tool(
            f"{self.prefix}{remote}"[:64],
            spec.description or remote,
            dict(spec.inputSchema or {}),
            handler,
            mutates=not read_only,
            category="mcp",
            source="mcp",
        )

    async def close(self) -> None:
        await self._stack.aclose()

    async def __aenter__(self) -> MCPToolset:
        return self

    async def __aexit__(self, *exc: object) -> None:
        await self.close()
