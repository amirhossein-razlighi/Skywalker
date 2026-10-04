"""Following the engine's live event stream (``events_poll``)."""

from __future__ import annotations

import asyncio
import contextlib
from typing import TYPE_CHECKING, Any

from pydantic import BaseModel, ConfigDict

from .protocol import Connection

if TYPE_CHECKING:  # pragma: no cover
    from .client import AsyncEngine


class Event(BaseModel):
    """One engine event. ``type`` is tool, studio, play_state, selection, scene, assets, tool_host, ...;
    studio events carry ``kind`` (message, task, feedback, decision, loop, agent, playtest) and ``action``."""

    model_config = ConfigDict(extra="allow")

    seq: int = 0
    time: float = 0.0
    type: str = ""
    kind: str = ""
    action: str = ""
    actor: str = ""
    id: str = ""
    tool: str = ""
    summary: str = ""

    @property
    def topic(self) -> str:
        """``type`` or ``type.kind`` (e.g. ``studio.message``)."""
        return f"{self.type}.{self.kind}" if self.kind else self.type

    @property
    def extra(self) -> dict[str, Any]:
        return dict(self.model_extra or {})

    def get(self, key: str, default: Any = None) -> Any:
        return (self.model_extra or {}).get(key, getattr(self, key, default))


class EventStream:
    """Async iterator over engine events, resumable by ``cursor``.

    On a socket (editor, ``skywalker serve``) it long-polls on a connection of its own, so events arrive
    as soon as they happen. On stdio it polls between calls.
    """

    def __init__(
        self,
        engine: AsyncEngine,
        *,
        since: int | None,
        types: list[str] | None,
        actors: list[str] | None,
        exclude_actors: list[str] | None,
        wait_ms: int,
        poll_interval: float,
    ) -> None:
        self.engine = engine
        self.cursor = since
        self.types = types
        self.actors = actors
        self.exclude_actors = exclude_actors
        self.wait_ms = max(0, min(wait_ms, 25000))
        self.poll_interval = poll_interval
        self._buffer: list[Event] = []
        self._conn: Connection | None = None
        self._owned = False
        self._closed = False
        self.truncated = False

    def __aiter__(self) -> EventStream:
        return self

    async def _connection(self) -> Connection:
        if self._conn is None:
            if self.engine.supports_sessions:
                self._conn = await self.engine.new_connection("events")
                self._owned = True
            else:
                self._conn = await self.engine._session()
        return self._conn

    async def poll(self, wait_ms: int | None = None) -> list[Event]:
        """One poll: the events after the cursor (possibly none)."""
        conn = await self._connection()
        if self.cursor is None:
            first = await conn.call_tool("events_poll", {"since": 0, "limit": 1})
            self.cursor = int(first.data.get("last_seq", 0))
        args: dict[str, Any] = {"since": self.cursor, "limit": 500}
        if self.types:
            args["types"] = self.types
        if self.actors:
            args["actors"] = self.actors
        if self.exclude_actors:
            args["exclude_actors"] = self.exclude_actors
        long_poll = self.engine.mode != "stdio"
        wait = self.wait_ms if wait_ms is None else wait_ms
        if long_poll and wait:
            args["wait_ms"] = wait
        result = await conn.call_tool("events_poll", args)
        result.raise_for_error()
        data = result.data
        self.cursor = int(data.get("next", self.cursor))
        self.truncated = self.truncated or bool(data.get("truncated"))
        return [Event.model_validate(e) for e in data.get("events", [])]

    async def __anext__(self) -> Event:
        while not self._buffer:
            if self._closed:
                raise StopAsyncIteration
            got = await self.poll()
            if got:
                self._buffer.extend(got)
            elif self.engine.mode == "stdio" or not self.wait_ms:
                await asyncio.sleep(self.poll_interval)
        return self._buffer.pop(0)

    async def next(self, timeout: float | None = None) -> Event | None:
        """The next event, or None after ``timeout`` seconds (the long poll is shortened, never abandoned)."""
        loop = asyncio.get_running_loop()
        deadline = None if timeout is None else loop.time() + timeout
        while not self._buffer:
            if self._closed:
                return None
            remaining = None if deadline is None else deadline - loop.time()
            if remaining is not None and remaining <= 0:
                return None
            wait = self.wait_ms if remaining is None else int(min(self.wait_ms, remaining * 1000))
            got = await self.poll(wait)
            if got:
                self._buffer.extend(got)
            elif self.engine.mode == "stdio" or not wait:
                await asyncio.sleep(self.poll_interval if remaining is None else min(self.poll_interval, remaining))
        return self._buffer.pop(0)

    async def close(self) -> None:
        self._closed = True
        if self._owned and self._conn is not None:
            with contextlib.suppress(Exception):
                await self._conn.close()
        self._conn = None

    async def __aenter__(self) -> EventStream:
        return self

    async def __aexit__(self, *exc: object) -> None:
        await self.close()
