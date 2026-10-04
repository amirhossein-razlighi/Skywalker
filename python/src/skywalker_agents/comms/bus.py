"""Message buses: direct messages, topics (pub/sub), threads, request/reply and handoffs.

:class:`StudioBus` is the default: messages are studio messages (``studio/messages.jsonl``), so the
editor's Studio panel shows every exchange and threads persist with the project; subscribers are
fed from the engine's event stream. :class:`LocalBus` is the same API in memory (tests, offline).
"""

from __future__ import annotations

import asyncio
import contextlib
import itertools
import json
import time
from collections.abc import Sequence
from pathlib import Path
from typing import TYPE_CHECKING, Any, Protocol

from ..errors import ToolError
from .messages import HANDOFF, REQUEST, RESULT, Envelope

if TYPE_CHECKING:  # pragma: no cover
    from ..engine.client import AsyncEngine
    from ..engine.events import EventStream


class Subscription:
    """An async iterator of envelopes that match a filter."""

    def __init__(
        self,
        bus: _Dispatch,
        *,
        agent: str | None,
        topic: str | None,
        kinds: Sequence[str] | None,
        thread: str | None,
        include_own: bool,
    ) -> None:
        self._bus = bus
        self.agent = agent
        self.topic = topic
        self.kinds = set(kinds or [])
        self.thread = thread
        self.include_own = include_own
        self.queue: asyncio.Queue[Envelope] = asyncio.Queue()

    def matches(self, env: Envelope) -> bool:
        if self.kinds and env.kind not in self.kinds:
            return False
        if self.thread and env.root != self.thread:
            return False
        if self.topic and env.topic != self.topic.lstrip("#"):
            return False
        if self.agent:
            if env.sender == self.agent and not self.include_own:
                return False
            broadcast = not env.to and not env.mentions and env.topic == "general"
            if not (env.addressed_to(self.agent) or broadcast or self.topic or self.thread):
                return False
        return True

    def __aiter__(self) -> Subscription:
        return self

    async def __anext__(self) -> Envelope:
        return await self.queue.get()

    async def get(self, timeout: float | None = None) -> Envelope | None:
        try:
            return await asyncio.wait_for(self.queue.get(), timeout)
        except asyncio.TimeoutError:
            return None

    def close(self) -> None:
        self._bus._unsubscribe(self)


class _Dispatch:
    def __init__(self) -> None:
        self._subs: list[Subscription] = []

    def _publish(self, env: Envelope) -> None:
        for s in list(self._subs):
            if s.matches(env):
                s.queue.put_nowait(env)

    def _unsubscribe(self, sub: Subscription) -> None:
        with contextlib.suppress(ValueError):
            self._subs.remove(sub)

    def subscribe(
        self,
        *,
        agent: str | None = None,
        topic: str | None = None,
        kinds: Sequence[str] | None = None,
        thread: str | None = None,
        include_own: bool = False,
    ) -> Subscription:
        """Messages for ``agent`` (addressed, mentioned or broadcast), on ``topic``, of ``kinds``, in ``thread``."""
        sub = Subscription(self, agent=agent, topic=topic, kinds=kinds, thread=thread, include_own=include_own)
        self._subs.append(sub)
        self._started()
        return sub

    def _started(self) -> None:
        pass


class MessageBus(Protocol):
    async def send(self, env: Envelope) -> Envelope: ...

    async def thread(self, root: str) -> list[Envelope]: ...

    async def inbox(self, agent: str, *, unread_only: bool = True, limit: int = 30) -> list[Envelope]: ...

    def subscribe(
        self,
        *,
        agent: str | None = None,
        topic: str | None = None,
        kinds: Sequence[str] | None = None,
        thread: str | None = None,
        include_own: bool = False,
    ) -> Subscription: ...

    async def close(self) -> None: ...


class _Conversations:
    """Request/reply and handoff on top of send + subscribe (shared by both buses)."""

    async def send(self, env: Envelope) -> Envelope:  # pragma: no cover - implemented by subclasses
        raise NotImplementedError

    def subscribe(
        self,
        *,
        agent: str | None = None,
        topic: str | None = None,
        kinds: Sequence[str] | None = None,
        thread: str | None = None,
        include_own: bool = False,
    ) -> Subscription:  # pragma: no cover
        raise NotImplementedError

    async def tell(
        self,
        sender: str,
        to: str | Sequence[str],
        text: str,
        *,
        kind: str = "inform",
        data: dict[str, Any] | None = None,
        topic: str = "general",
    ) -> Envelope:
        """A direct message."""
        targets = [to] if isinstance(to, str) else list(to)
        return await self.send(Envelope(sender=sender, to=targets, text=text, kind=kind, data=data or {}, topic=topic))

    async def publish(
        self, sender: str, topic: str, text: str, *, kind: str = "inform", data: dict[str, Any] | None = None
    ) -> Envelope:
        """A message on a topic (studio channel) for every subscriber."""
        return await self.send(Envelope(sender=sender, topic=topic.lstrip("#"), text=text, kind=kind, data=data or {}))

    async def reply(
        self, to: Envelope, sender: str, text: str, *, kind: str = RESULT, data: dict[str, Any] | None = None
    ) -> Envelope:
        return await self.send(
            Envelope(
                sender=sender, reply_to=to.id, to=[to.sender], text=text, kind=kind, data=data or {}, topic=to.topic
            )
        )

    async def request(
        self,
        sender: str,
        to: str,
        text: str,
        *,
        data: dict[str, Any] | None = None,
        kind: str = REQUEST,
        timeout: float | None = 300,
        topic: str = "general",
    ) -> Envelope | None:
        """Sends a request and waits for the first reply in its thread (None on timeout)."""
        sub = self.subscribe(agent=sender)  # before sending: a fast reply must not be missed
        try:
            await self.ready()
            msg = await self.send(Envelope(sender=sender, to=[to], text=text, kind=kind, data=data or {}, topic=topic))
            deadline = time.monotonic() + timeout if timeout else None
            while True:
                remaining = None if deadline is None else deadline - time.monotonic()
                if remaining is not None and remaining <= 0:
                    return None
                env = await sub.get(remaining)
                if env is None:
                    return None
                if env.root == msg.id and env.id != msg.id and env.sender != sender:
                    return env
        finally:
            sub.close()

    async def ready(self) -> None:
        """Subscriptions made before this returns see every later message."""

    async def handoff(
        self,
        sender: str,
        to: str,
        task: str,
        *,
        context: dict[str, Any] | None = None,
        board: Any | None = None,
        acceptance: Sequence[str] = (),
    ) -> Envelope:
        """Hands work to another agent: a ``handoff`` message, plus a board task when ``board`` is given."""
        refs: dict[str, Any] = {}
        data = {"task": task, "context": context or {}, "from": sender}
        if board is not None:
            t = await board.post(
                task,
                description=json.dumps(context or {}, default=str)[:2000] if context else "",
                acceptance=acceptance,
                assignee=to,
                by=sender,
            )
            refs["task"] = t["id"]
            data["board_task"] = t["id"]
        return await self.send(
            Envelope(sender=sender, to=[to], text=f"Handoff: {task}", kind=HANDOFF, data=data, refs=refs)
        )


class StudioBus(_Dispatch, _Conversations):
    """Messages through the engine's studio (persisted with the project, visible in the editor)."""

    def __init__(self, engine: AsyncEngine) -> None:
        _Dispatch.__init__(self)
        self.engine = engine
        self._stream: EventStream | None = None
        self._task: asyncio.Task[None] | None = None

    async def send(self, env: Envelope) -> Envelope:
        caller = self.engine.as_agent(env.sender) if env.sender else self.engine
        res = await caller.call("studio_message_send", env.to_studio_args())
        if res.is_error:
            raise ToolError("studio_message_send", res.error_code, res.error_message, res.error_hint)
        return Envelope.from_studio(res.data)

    async def thread(self, root: str) -> list[Envelope]:
        res = await self.engine.call("studio_inbox", {"thread": root, "limit": 500}, check=True)
        return [Envelope.from_studio(m) for m in res.data.get("messages", [])]

    async def topic(self, topic: str, *, after: str = "", limit: int = 100) -> list[Envelope]:
        args: dict[str, Any] = {"channel": topic, "limit": limit}
        if after:
            args["after"] = after
        res = await self.engine.call("studio_inbox", args, check=True)
        return [Envelope.from_studio(m) for m in res.data.get("messages", [])]

    async def inbox(self, agent: str, *, unread_only: bool = True, limit: int = 30) -> list[Envelope]:
        res = await self.engine.as_agent(agent).call(
            "studio_inbox", {"unread_only": unread_only, "limit": limit}, check=True
        )
        return [Envelope.from_studio(m) for m in res.data.get("messages", [])]

    def _started(self) -> None:
        if self._task is None:
            self._stream = self.engine.events(types=["studio.message"])
            self._task = asyncio.create_task(self._pump(), name="studio-bus")

    async def _pump(self) -> None:
        assert self._stream is not None
        async for ev in self._stream:
            msg = ev.get("message")
            if isinstance(msg, dict):
                self._publish(Envelope.from_studio(msg))

    async def ready(self) -> None:
        """Waits until the subscription stream has its cursor (messages sent after this are seen)."""
        self._started()
        assert self._stream is not None
        for _ in range(500):
            if self._stream.cursor is not None:
                return
            await asyncio.sleep(0.01)

    async def close(self) -> None:
        if self._task is not None:
            self._task.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await self._task
            self._task = None
        if self._stream is not None:
            await self._stream.close()
            self._stream = None


class LocalBus(_Dispatch, _Conversations):
    """In-memory bus with the same semantics (optionally appended to a JSONL file)."""

    def __init__(self, path: str | Path | None = None) -> None:
        _Dispatch.__init__(self)
        self.messages: list[Envelope] = []
        self._ids = itertools.count(1)
        self.path = Path(path) if path else None
        self._read: dict[str, int] = {}
        if self.path and self.path.exists():
            for line in self.path.read_text().splitlines():
                if line.strip():
                    self.messages.append(Envelope.model_validate_json(line))
            self._ids = itertools.count(len(self.messages) + 1)

    async def send(self, env: Envelope) -> Envelope:
        env = env.model_copy(deep=True)
        env.id = f"L-{next(self._ids)}"
        env.at = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        if env.reply_to:
            parent = next((m for m in self.messages if m.id == env.reply_to), None)
            if parent is None:
                raise ToolError("send", "not_found", f"no message {env.reply_to}")
            env.thread = parent.root
            if parent.sender not in env.to:
                env.to.append(parent.sender)
        env.to = [t for t in env.to if t != env.sender]
        env.reply_to = ""
        self.messages.append(env)
        if self.path:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with self.path.open("a") as f:
                f.write(env.model_dump_json() + "\n")
        self._publish(env)
        await asyncio.sleep(0)
        return env

    async def thread(self, root: str) -> list[Envelope]:
        return [m for m in self.messages if m.root == root]

    async def inbox(self, agent: str, *, unread_only: bool = True, limit: int = 30) -> list[Envelope]:
        start = self._read.get(agent, 0) if unread_only else 0
        mine = [
            (i, m)
            for i, m in enumerate(self.messages)
            if i >= start
            and m.sender != agent
            and (m.addressed_to(agent) or (not m.to and not m.mentions and m.topic == "general"))
        ]
        if unread_only:
            self._read[agent] = len(self.messages)
        return [m for _, m in mine][-limit:]

    async def close(self) -> None:
        pass


async def collect(sub: Subscription, n: int, timeout: float = 5.0) -> list[Envelope]:
    """Up to ``n`` envelopes from a subscription within ``timeout`` (tests, scripts)."""
    out: list[Envelope] = []
    deadline = time.monotonic() + timeout
    while len(out) < n:
        env = await sub.get(max(0.0, deadline - time.monotonic()))
        if env is None:
            break
        out.append(env)
    return out
