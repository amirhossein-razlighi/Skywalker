"""JSON-RPC 2.0 over newline-delimited streams: the engine's MCP framing on stdio and its socket.

One :class:`JsonRpcConnection` is one MCP session. The engine answers requests on a connection in
order, one at a time, so long polls (events, tool-host calls) get connections of their own.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
from collections.abc import Awaitable, Callable
from typing import Any, Protocol

from .._version import __version__
from ..errors import EngineConnectionError, ProtocolError
from .results import ToolResult

PROTOCOL_VERSION = "2025-06-18"
_LINE_LIMIT = 256 * 1024 * 1024  # captures arrive as base64 inside one line


class Connection(Protocol):
    """What the client needs from a transport: one attributed MCP session."""

    client_name: str

    async def call_tool(
        self, name: str, arguments: dict[str, Any] | None = None, timeout: float | None = None
    ) -> ToolResult: ...

    async def request(self, method: str, params: dict[str, Any] | None = None, timeout: float | None = None) -> Any: ...

    async def close(self) -> None: ...

    @property
    def closed(self) -> bool: ...


class JsonRpcConnection:
    """An MCP client session over a pair of asyncio streams."""

    def __init__(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
        client_name: str,
        on_close: Callable[[], Awaitable[None]] | None = None,
    ) -> None:
        self.client_name = client_name
        self._reader = reader
        self._writer = writer
        self._on_close = on_close
        self._next_id = 1
        self._pending: dict[int, asyncio.Future[Any]] = {}
        self._closed = False
        self._write_lock = asyncio.Lock()
        self._reader_task = asyncio.create_task(self._read_loop(), name=f"sky-rpc-{client_name}")
        self.server_info: dict[str, Any] = {}
        self.instructions = ""

    @property
    def closed(self) -> bool:
        return self._closed

    async def initialize(self) -> dict[str, Any]:
        result = await self.request(
            "initialize",
            {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": self.client_name, "version": __version__},
            },
            timeout=30,
        )
        await self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        self.server_info = dict(result.get("serverInfo") or {})
        self.instructions = str(result.get("instructions") or "")
        return dict(result)

    async def request(self, method: str, params: dict[str, Any] | None = None, timeout: float | None = None) -> Any:
        if self._closed:
            raise EngineConnectionError(f"connection '{self.client_name}' is closed")
        rid = self._next_id
        self._next_id += 1
        fut: asyncio.Future[Any] = asyncio.get_running_loop().create_future()
        self._pending[rid] = fut
        msg: dict[str, Any] = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        try:
            await self._send(msg)
            if timeout is None:
                return await fut
            return await asyncio.wait_for(fut, timeout)
        finally:
            self._pending.pop(rid, None)

    async def call_tool(
        self, name: str, arguments: dict[str, Any] | None = None, timeout: float | None = None
    ) -> ToolResult:
        result = await self.request("tools/call", {"name": name, "arguments": arguments or {}}, timeout=timeout)
        return ToolResult.from_mcp(name, result)

    async def _send(self, msg: dict[str, Any]) -> None:
        data = (json.dumps(msg, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
        async with self._write_lock:
            try:
                self._writer.write(data)
                await self._writer.drain()
            except (ConnectionError, OSError) as e:
                await self._fail(EngineConnectionError(f"engine connection lost: {e}"))
                raise EngineConnectionError(f"engine connection lost: {e}") from e

    async def _read_loop(self) -> None:
        error: Exception = EngineConnectionError("the engine closed the connection")
        try:
            while True:
                line = await self._reader.readline()
                if not line:
                    break
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    continue
                for item in msg if isinstance(msg, list) else [msg]:
                    self._dispatch(item)
        except (ConnectionError, OSError, ValueError) as e:  # ValueError: line over the limit
            error = EngineConnectionError(f"engine connection lost: {e}")
        except asyncio.CancelledError:
            error = EngineConnectionError("connection closed")
        await self._fail(error)

    def _dispatch(self, msg: Any) -> None:
        if not isinstance(msg, dict) or "id" not in msg or "method" in msg:
            return  # notifications / server requests: not used by the engine today
        fut = self._pending.get(msg["id"])
        if fut is None or fut.done():
            return
        if "error" in msg:
            err = msg["error"] or {}
            fut.set_exception(ProtocolError(str(err.get("message", "error")), err.get("code")))
        else:
            fut.set_result(msg.get("result"))

    async def _fail(self, error: Exception) -> None:
        if self._closed:
            return
        self._closed = True
        for fut in self._pending.values():
            if not fut.done():
                fut.set_exception(error)
        self._pending.clear()
        with contextlib.suppress(Exception):
            self._writer.close()
        if self._on_close is not None:
            with contextlib.suppress(Exception):
                await self._on_close()

    async def close(self) -> None:
        if self._closed:
            return
        self._reader_task.cancel()
        with contextlib.suppress(asyncio.CancelledError, Exception):
            await self._reader_task
        await self._fail(EngineConnectionError("connection closed"))
        with contextlib.suppress(Exception):
            await self._writer.wait_closed()


async def open_unix(path: str, client_name: str) -> JsonRpcConnection:
    """Connects to an engine socket (the editor's, or ``skywalker serve``) and initializes MCP."""
    try:
        reader, writer = await asyncio.open_unix_connection(path, limit=_LINE_LIMIT)
    except (FileNotFoundError, ConnectionRefusedError, OSError) as e:
        raise EngineConnectionError(
            f"cannot connect to {path}: {e} (is the editor running with External Agents on, or `skywalker serve`?)"
        ) from e
    conn = JsonRpcConnection(reader, writer, client_name)
    await conn.initialize()
    return conn
