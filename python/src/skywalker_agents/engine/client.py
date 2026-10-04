"""The engine client: one live engine (the editor, ``skywalker serve``, stdio or a fake), many sessions.

Every agent gets its own *session* (an MCP connection named ``sky-agents/<agent id>``), so its edits,
messages and tool calls are attributed to it in the editor's Activity feed and the Studio panel,
exactly like the editor's own crew. Long polls (events, tool hosting) use dedicated connections.

Async first (:class:`AsyncEngine`); :class:`Engine` is the same API for synchronous code.
"""

from __future__ import annotations

import asyncio
import contextlib
import os
import threading
from collections.abc import AsyncIterator, Awaitable, Callable, Coroutine, Iterator
from typing import TYPE_CHECKING, Any, Literal, TypeVar

from pydantic import BaseModel, ConfigDict, Field

from ..errors import EngineConnectionError
from .fake import FakeEngine
from .process import ServeProcess, default_editor_socket, open_stdio
from .protocol import Connection, open_unix
from .results import ToolResult

if TYPE_CHECKING:  # pragma: no cover
    from ..tools.base import Tool
    from ._generated import AsyncTools, Tools
    from .events import Event, EventStream
    from .host import ToolHostServer

T = TypeVar("T")
Mode = Literal["socket", "stdio", "fake"]


class ToolSpec(BaseModel):
    """An engine tool as listed by ``tools/list``."""

    model_config = ConfigDict(extra="allow", populate_by_name=True)

    name: str
    title: str = ""
    description: str = ""
    input_schema: dict[str, Any] = Field(default_factory=dict, alias="inputSchema")
    annotations: dict[str, Any] = Field(default_factory=dict)
    meta: dict[str, Any] = Field(default_factory=dict, alias="_meta")

    @property
    def category(self) -> str:
        return str(self.meta.get("skywalker/category", ""))

    @property
    def read_only(self) -> bool:
        return bool(self.annotations.get("readOnlyHint", False))

    @property
    def mutates(self) -> bool:
        return not self.read_only

    @property
    def open_world(self) -> bool:
        return bool(self.annotations.get("openWorldHint", False))

    def accepts(self, arg: str) -> bool:
        return arg in (self.input_schema.get("properties") or {})


class AsyncEngine:
    """A live engine shared by many agents. Create with :meth:`attach`, :meth:`spawn`, :meth:`auto` or :meth:`fake`."""

    def __init__(
        self,
        connect: Callable[[str], Awaitable[Connection]],
        *,
        mode: Mode,
        client_name: str = "sky-agents",
        project: str | None = None,
        on_close: Callable[[], Awaitable[None]] | None = None,
        shared: Connection | None = None,
    ) -> None:
        self._connect = connect
        self.mode: Mode = mode
        self.client_name = client_name
        self.project = os.path.abspath(project) if project else None
        self._on_close = on_close
        self._main: Connection | None = shared
        self._conns: dict[str, Connection] = {}
        self._tool_specs: dict[str, ToolSpec] | None = None
        self._lock = asyncio.Lock()
        self._hosts: list[ToolHostServer] = []
        self.fake_engine: FakeEngine | None = None

    # ------------------------------------------------------------------ construction
    @classmethod
    async def attach(
        cls, socket: str | None = None, *, client_name: str = "sky-agents", project: str | None = None
    ) -> AsyncEngine:
        """Attach to a running editor (``~/.skywalker/editor.sock``) or ``skywalker serve`` socket."""
        path = socket or os.environ.get("SKYWALKER_SOCKET") or default_editor_socket()
        engine = cls(lambda name: open_unix(path, name), mode="socket", client_name=client_name, project=project)
        engine.socket_path = path
        await engine._session()  # fail early if nothing listens
        return engine

    @classmethod
    async def spawn(
        cls,
        project: str | os.PathLike[str] = ".",
        *,
        scene: str | None = None,
        binary: str | None = None,
        client_name: str = "sky-agents",
        transport: Literal["serve", "stdio"] = "serve",
    ) -> AsyncEngine:
        """Start a headless engine on a project. ``serve`` (default) gives a socket every agent can share;
        ``stdio`` runs ``skywalker mcp`` (one connection: no long polls or tool hosting)."""
        if transport == "stdio":
            conn = await open_stdio(project, scene=scene, binary=binary, client_name=client_name)

            async def same(_: str) -> Connection:
                return conn

            return cls(
                same,
                mode="stdio",
                client_name=client_name,
                project=os.fspath(project),
                shared=conn,
                on_close=conn.close,
            )
        proc = await ServeProcess.start(project, scene=scene, binary=binary)
        engine = cls(
            lambda name: open_unix(proc.socket, name),
            mode="socket",
            client_name=client_name,
            project=os.fspath(project),
            on_close=proc.stop,
        )
        engine.socket_path = proc.socket
        engine.process = proc
        try:
            await engine._session()
        except Exception:
            await proc.stop()
            raise
        return engine

    @classmethod
    async def auto(
        cls,
        project: str | os.PathLike[str] = ".",
        *,
        socket: str | None = None,
        binary: str | None = None,
        client_name: str = "sky-agents",
    ) -> AsyncEngine:
        """Attach to the running editor when there is one, otherwise spawn ``skywalker serve`` on ``project``."""
        try:
            return await cls.attach(socket, client_name=client_name, project=os.fspath(project))
        except EngineConnectionError:
            return await cls.spawn(project, binary=binary, client_name=client_name)

    @classmethod
    def fake(cls, engine: FakeEngine | None = None, *, client_name: str = "sky-agents") -> AsyncEngine:
        """An in-process fake engine (tests, offline development)."""
        fake = engine or FakeEngine()

        async def connect(name: str) -> Connection:
            return fake.connect(name)

        out = cls(connect, mode="fake", client_name=client_name)
        out.fake_engine = fake
        return out

    socket_path: str | None = None
    process: ServeProcess | None = None

    # ------------------------------------------------------------------ sessions
    @property
    def supports_sessions(self) -> bool:
        """True when each agent can have its own attributed connection (not stdio)."""
        return self.mode != "stdio"

    async def _session(self) -> Connection:
        if self._main is None or self._main.closed:
            self._main = await self._connect(self.client_name)
        return self._main

    async def connection(self, purpose: str, *, lane: str = "") -> Connection:
        """A connection of its own named ``<client>/<purpose>`` (stdio: the shared one).

        ``lane`` opens a parallel connection with the same identity (client ``<client>-<lane>/<purpose>``):
        the engine answers one request per connection at a time, so work done *while* a call on the main
        lane waits (a Python tool serving that call) must not queue behind it.
        """
        if not self.supports_sessions:
            return await self._session()
        key = f"{lane}:{purpose}"
        async with self._lock:
            conn = self._conns.get(key)
            if conn is None or conn.closed:
                prefix = f"{self.client_name}-{lane}" if lane else self.client_name
                conn = await self._connect(f"{prefix}/{purpose}")
                self._conns[key] = conn
            return conn

    async def new_connection(self, purpose: str) -> Connection:
        """A fresh, unshared connection (caller closes it). Stdio: the shared one."""
        if not self.supports_sessions:
            return await self._session()
        return await self._connect(f"{self.client_name}/{purpose}")

    def as_agent(self, agent_id: str, *, lane: str = "", meta: dict[str, Any] | None = None) -> AgentSession:
        """Calls attributed to a roster member: ``mcp:sky-agents/<id>`` in the Activity feed (``lane``: a
        parallel connection with the same identity, see :meth:`connection`; ``meta``: MCP ``_meta`` sent with
        every call, e.g. ``{"skywalker/call_id": ...}`` for the callbacks of a hosted tool call)."""
        return AgentSession(self, agent_id, lane=lane, meta=meta)

    # ------------------------------------------------------------------ calls
    async def call(
        self,
        tool: str,
        args: dict[str, Any] | None = None,
        /,
        *,
        check: bool = False,
        timeout: float | None = None,
        **kwargs: Any,
    ) -> ToolResult:
        """Calls a tool as this client. ``check=True`` raises :class:`ToolError` on an error result."""
        merged = {**(args or {}), **kwargs}
        conn = await self._session()
        result = await conn.call_tool(tool, merged, timeout=timeout)
        return result.raise_for_error() if check else result

    async def list_tools(self, refresh: bool = False) -> list[ToolSpec]:
        if self._tool_specs is None or refresh:
            conn = await self._session()
            payload = await conn.request("tools/list", {})
            self._tool_specs = {t["name"]: ToolSpec.model_validate(t) for t in payload.get("tools", [])}
        return list(self._tool_specs.values())

    async def tool_spec(self, name: str) -> ToolSpec | None:
        if self._tool_specs is None or name not in self._tool_specs:
            await self.list_tools(refresh=True)
        assert self._tool_specs is not None
        return self._tool_specs.get(name)

    @property
    def tools(self) -> AsyncTools:
        """Typed wrappers for every engine tool: ``await engine.tools.viewport_capture(width=640)``."""
        from ._generated import AsyncTools

        return AsyncTools(self)

    # ------------------------------------------------------------------ events and tool hosting
    def events(
        self,
        *,
        since: int | None = None,
        types: list[str] | None = None,
        actors: list[str] | None = None,
        exclude_actors: list[str] | None = None,
        wait_ms: int = 20000,
        poll_interval: float = 0.25,
    ) -> EventStream:
        """Follow the engine's event stream (async iterator of :class:`Event`). ``since=None`` starts now."""
        from .events import EventStream

        return EventStream(
            self,
            since=since,
            types=types,
            actors=actors,
            exclude_actors=exclude_actors,
            wait_ms=wait_ms,
            poll_interval=poll_interval,
        )

    async def host_tools(
        self,
        tools: list[Tool],
        *,
        label: str | None = None,
        concurrency: int = 8,
        ttl_seconds: float = 60,
        capabilities: dict[str, dict[str, Any]] | None = None,
        limits: dict[str, dict[str, Any]] | None = None,
    ) -> ToolHostServer:
        """Serve Python tools to every agent on this engine as ``py_<name>`` (socket and fake modes).

        ``capabilities`` and ``limits`` (by tool name, with or without ``py_``) override what each tool
        declares (:attr:`Tool.capabilities`, :attr:`Tool.limits`): the engine tools it may call back into while
        serving a call (``{"calls": [...], "mutate": bool, "network": bool}``) and its ``timeout_ms``,
        ``max_output_bytes`` and ``max_calls`` (docs/CUSTOM_TOOLS.md). ``server.status`` then says whether each
        tool is ``active`` or waits for a human's approval (``pending_approval``).
        """
        from .host import ToolHostServer

        server = ToolHostServer(
            self,
            tools,
            label=label or self.client_name,
            concurrency=concurrency,
            ttl_seconds=ttl_seconds,
            capabilities=capabilities,
            limits=limits,
        )
        await server.start()
        self._hosts.append(server)
        return server

    # ------------------------------------------------------------------ lifecycle
    async def close(self) -> None:
        for h in self._hosts:
            with contextlib.suppress(Exception):
                await h.stop()
        self._hosts.clear()
        for c in list(self._conns.values()):
            with contextlib.suppress(Exception):
                await c.close()
        self._conns.clear()
        if self._main is not None:
            with contextlib.suppress(Exception):
                await self._main.close()
            self._main = None
        if self._on_close is not None:
            await self._on_close()
            self._on_close = None

    async def __aenter__(self) -> AsyncEngine:
        return self

    async def __aexit__(self, *exc: object) -> None:
        await self.close()


class AgentSession:
    """An engine handle attributed to one roster member (its own connection; ``as`` on stdio)."""

    def __init__(
        self, engine: AsyncEngine, agent_id: str, *, lane: str = "", meta: dict[str, Any] | None = None
    ) -> None:
        self.engine = engine
        self.agent_id = agent_id
        self.lane = lane
        self.meta = dict(meta or {})  # MCP _meta sent with every call (a hosted tool's call id)

    @property
    def actor(self) -> str:
        prefix = f"{self.engine.client_name}-{self.lane}" if self.lane else self.engine.client_name
        return f"mcp:{prefix}/{self.agent_id}"

    async def call(
        self,
        tool: str,
        args: dict[str, Any] | None = None,
        /,
        *,
        check: bool = False,
        timeout: float | None = None,
        **kwargs: Any,
    ) -> ToolResult:
        merged = {**(args or {}), **kwargs}
        if not self.engine.supports_sessions and "as" not in merged:
            spec = await self.engine.tool_spec(tool)
            if spec is not None and spec.accepts("as"):
                merged["as"] = self.agent_id
        conn = await self.engine.connection(self.agent_id, lane=self.lane)
        if self.meta:
            result = await conn.call_tool(tool, merged, timeout=timeout, meta=self.meta)
        else:
            result = await conn.call_tool(tool, merged, timeout=timeout)
        return result.raise_for_error() if check else result

    @property
    def tools(self) -> AsyncTools:
        from ._generated import AsyncTools

        return AsyncTools(self)


# ---------------------------------------------------------------------- synchronous facade
class _Portal:
    """An event loop on a daemon thread that runs the async client for synchronous callers."""

    def __init__(self) -> None:
        self.loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self.loop.run_forever, name="sky-agents-loop", daemon=True)
        self._thread.start()

    def run(self, aw: Awaitable[T], timeout: float | None = None) -> T:
        async def wrap() -> T:
            return await aw

        return asyncio.run_coroutine_threadsafe(wrap(), self.loop).result(timeout)

    def close(self) -> None:
        if self.loop.is_running():
            self.loop.call_soon_threadsafe(self.loop.stop)
            self._thread.join(5)
        if not self.loop.is_running():
            self.loop.close()


class Engine:
    """Synchronous engine client (same API as :class:`AsyncEngine`; ``.aio`` gives the async one)."""

    def __init__(self, aio: AsyncEngine, portal: _Portal) -> None:
        self.aio = aio
        self._portal = portal

    @classmethod
    def attach(
        cls, socket: str | None = None, *, client_name: str = "sky-agents", project: str | None = None
    ) -> Engine:
        portal = _Portal()
        return cls(portal.run(AsyncEngine.attach(socket, client_name=client_name, project=project)), portal)

    @classmethod
    def spawn(
        cls,
        project: str | os.PathLike[str] = ".",
        *,
        scene: str | None = None,
        binary: str | None = None,
        client_name: str = "sky-agents",
        transport: Literal["serve", "stdio"] = "serve",
    ) -> Engine:
        portal = _Portal()
        aio = portal.run(
            AsyncEngine.spawn(project, scene=scene, binary=binary, client_name=client_name, transport=transport)
        )
        return cls(aio, portal)

    @classmethod
    def auto(
        cls,
        project: str | os.PathLike[str] = ".",
        *,
        socket: str | None = None,
        binary: str | None = None,
        client_name: str = "sky-agents",
    ) -> Engine:
        portal = _Portal()
        return cls(portal.run(AsyncEngine.auto(project, socket=socket, binary=binary, client_name=client_name)), portal)

    @classmethod
    def fake(cls, engine: FakeEngine | None = None, *, client_name: str = "sky-agents") -> Engine:
        return cls(AsyncEngine.fake(engine, client_name=client_name), _Portal())

    @property
    def mode(self) -> Mode:
        return self.aio.mode

    def run(self, coro: Coroutine[Any, Any, T], timeout: float | None = None) -> T:
        """Runs a coroutine on the client's loop (for async helpers from sync code)."""
        return self._portal.run(coro, timeout)

    def call(
        self,
        tool: str,
        args: dict[str, Any] | None = None,
        /,
        *,
        check: bool = False,
        timeout: float | None = None,
        **kwargs: Any,
    ) -> ToolResult:
        return self._portal.run(self.aio.call(tool, args, check=check, timeout=timeout, **kwargs))

    def list_tools(self, refresh: bool = False) -> list[ToolSpec]:
        return self._portal.run(self.aio.list_tools(refresh))

    @property
    def tools(self) -> Tools:
        from ._generated import Tools

        return Tools(self)

    def as_agent(self, agent_id: str) -> SyncAgentSession:
        return SyncAgentSession(self, self.aio.as_agent(agent_id))

    def events(
        self,
        *,
        since: int | None = None,
        types: list[str] | None = None,
        actors: list[str] | None = None,
        exclude_actors: list[str] | None = None,
        wait_ms: int = 20000,
    ) -> Iterator[Event]:
        """Blocking iterator over engine events."""
        stream = self.aio.events(
            since=since, types=types, actors=actors, exclude_actors=exclude_actors, wait_ms=wait_ms
        )
        it: AsyncIterator[Event] = stream.__aiter__()
        try:
            while True:
                try:
                    yield self._portal.run(it.__anext__())
                except StopAsyncIteration:
                    return
        finally:
            self._portal.run(stream.close())

    def host_tools(
        self,
        tools: list[Tool],
        *,
        label: str | None = None,
        capabilities: dict[str, dict[str, Any]] | None = None,
        limits: dict[str, dict[str, Any]] | None = None,
    ) -> ToolHostServer:
        return self._portal.run(self.aio.host_tools(tools, label=label, capabilities=capabilities, limits=limits))

    def close(self) -> None:
        try:
            self._portal.run(self.aio.close(), timeout=30)
        finally:
            self._portal.close()

    def __enter__(self) -> Engine:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()


class SyncAgentSession:
    def __init__(self, engine: Engine, session: AgentSession) -> None:
        self._engine = engine
        self.aio = session
        self.agent_id = session.agent_id

    def call(
        self,
        tool: str,
        args: dict[str, Any] | None = None,
        /,
        *,
        check: bool = False,
        timeout: float | None = None,
        **kwargs: Any,
    ) -> ToolResult:
        return self._engine.run(self.aio.call(tool, args, check=check, timeout=timeout, **kwargs))

    @property
    def tools(self) -> Tools:
        from ._generated import Tools

        return Tools(self)
