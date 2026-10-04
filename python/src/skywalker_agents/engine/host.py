"""Serving Python tools to every agent on the engine as ``py_<name>`` tools (``tool_host_*``).

Claude Code attached to the editor, the editor's crew and other Python agents then call them like
any engine tool; the engine queues each call for this process, which runs the tool and replies.
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
from typing import TYPE_CHECKING, Any

from ..errors import EngineConnectionError, ToolError
from .protocol import Connection

if TYPE_CHECKING:  # pragma: no cover
    from ..tools.base import Tool
    from .client import AsyncEngine

log = logging.getLogger("skywalker_agents.host")


class ToolHostServer:
    """Registers tools with the engine and answers their calls until stopped."""

    def __init__(
        self, engine: AsyncEngine, tools: list[Tool], *, label: str, concurrency: int = 8, ttl_seconds: float = 60
    ) -> None:
        if engine.mode == "stdio":
            raise EngineConnectionError("tool hosting needs an agent socket (attach to the editor or use spawn())")
        self.engine = engine
        self.tools = {(t.name if t.name.startswith("py_") else f"py_{t.name}"): t for t in tools}
        self.label = label
        self.ttl_seconds = ttl_seconds
        self.host_id = ""
        self.served = 0
        self.failed = 0
        self._sem = asyncio.Semaphore(concurrency)
        self._poll_conn: Connection | None = None
        self._reply_conn: Connection | None = None
        self._task: asyncio.Task[None] | None = None
        self._inflight: set[asyncio.Task[None]] = set()
        self._stopping = False

    async def start(self) -> ToolHostServer:
        self._poll_conn = await self.engine.new_connection("tool-host")
        self._reply_conn = await self.engine.new_connection("tool-host-reply")
        await self._register()
        self._task = asyncio.create_task(self._loop(), name=f"tool-host-{self.host_id}")
        return self

    async def _register(self) -> None:
        assert self._reply_conn is not None
        specs = [
            {"name": name, "description": t.description, "input_schema": t.input_schema, "mutates": t.mutates}
            for name, t in self.tools.items()
        ]
        args: dict[str, Any] = {"tools": specs, "label": self.label, "ttl_seconds": self.ttl_seconds}
        if self.host_id:
            args["host"] = self.host_id
        res = await self._reply_conn.call_tool("tool_host_register", args)
        if res.is_error and self.host_id and res.error_code == "not_found":
            self.host_id = ""  # expired while we were away: register afresh
            args.pop("host")
            res = await self._reply_conn.call_tool("tool_host_register", args)
        res.raise_for_error()
        self.host_id = str(res.data["host"])
        log.info("serving %d tool(s) as %s", len(specs), self.host_id)

    async def _loop(self) -> None:
        assert self._poll_conn is not None
        backoff = 0.5
        while not self._stopping:
            try:
                res = await self._poll_conn.call_tool("tool_host_poll", {"host": self.host_id, "wait_ms": 15000})
                if res.is_error:
                    if res.error_code == "not_found":
                        await self._register()
                        continue
                    raise ToolError("tool_host_poll", res.error_code, res.error_message, res.error_hint)
                backoff = 0.5
                for call in res.data.get("calls", []):
                    task = asyncio.create_task(self._serve(call))
                    self._inflight.add(task)
                    task.add_done_callback(self._inflight.discard)
            except asyncio.CancelledError:
                raise
            except Exception as e:  # keep serving through transient errors
                if self.stopping():
                    return
                log.warning("tool host poll failed: %s", e)
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, 10)
                if self._poll_conn.closed:
                    with contextlib.suppress(Exception):
                        self._poll_conn = await self.engine.new_connection("tool-host")

    async def _serve(self, call: dict[str, Any]) -> None:
        from ..tools.base import ToolContext

        assert self._reply_conn is not None
        async with self._sem:
            tool = self.tools.get(str(call.get("tool")))
            actor = str(call.get("actor", ""))
            agent_id = actor.split("/", 1)[1] if "/" in actor else ""
            if tool is None:
                reply: dict[str, Any] = {
                    "text": f"error [unknown_tool]: {call.get('tool')} is not served here",
                    "is_error": True,
                }
            else:
                ctx = ToolContext(
                    agent_id=agent_id,
                    actor=actor,
                    call_id=str(call.get("call")),
                    engine=self.engine,
                    session=self.engine.as_agent(agent_id) if agent_id else None,
                )
                result = await tool.run(dict(call.get("args") or {}), ctx)
                reply = {"text": result.text, "is_error": result.is_error}
                if result.structured is not None:
                    reply["structured"] = result.structured
                imgs = [b.data for b in result.content if b.type == "image" and b.data]
                if imgs:
                    reply["images"] = imgs
            self.failed += int(bool(reply.get("is_error")))
            self.served += 1
            with contextlib.suppress(Exception):
                await self._reply_conn.call_tool(
                    "tool_host_reply", {"host": self.host_id, "call": call["call"], **reply}
                )

    def stopping(self) -> bool:
        return self._stopping

    async def stop(self) -> None:
        if self._stopping:
            return
        self._stopping = True
        if self._reply_conn is not None and self.host_id:
            with contextlib.suppress(Exception):
                await self._reply_conn.call_tool("tool_host_unregister", {"host": self.host_id})
        if self._task is not None:
            self._task.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await self._task
        for t in list(self._inflight):
            t.cancel()
        for c in (self._poll_conn, self._reply_conn):
            if c is not None:
                with contextlib.suppress(Exception):
                    await c.close()

    async def __aenter__(self) -> ToolHostServer:
        return self

    async def __aexit__(self, *exc: object) -> None:
        await self.stop()
