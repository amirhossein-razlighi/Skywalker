from __future__ import annotations

import asyncio
import json
import tempfile
from pathlib import Path
from typing import Any

import pytest
from pydantic import ValidationError

from skywalker_agents import AsyncEngine, Engine, FakeEngine, ToolError, ToolResult
from skywalker_agents.engine.codegen import generate, py_type
from skywalker_agents.engine.fake import TINY_PNG
from skywalker_agents.engine.protocol import JsonRpcConnection, open_unix
from skywalker_agents.errors import EngineConnectionError, ProtocolError


def test_tool_result_parses_engine_errors() -> None:
    r = ToolResult.from_mcp("entity_update", {
        "content": [{"type": "text", "text": "error [not_found]: no entity 'Lava'\nhint: did you mean 'Lava Pit'?"}],
        "isError": True})
    assert r.is_error
    assert r.error_code == "not_found"
    assert r.error_message == "no entity 'Lava'"
    assert r.error_hint == "did you mean 'Lava Pit'?"
    with pytest.raises(ToolError) as e:
        r.raise_for_error()
    assert e.value.code == "not_found"
    ok = ToolResult.ok("hi", {"a": 1}, images=[TINY_PNG])
    assert ok.images == [TINY_PNG]
    assert ok["a"] == 1
    assert ok.to_mcp()["structuredContent"] == {"a": 1}
    img = ok.pil_images()[0]
    assert img.size == (1, 1)


async def test_calls_are_attributed_per_agent_session(engine: AsyncEngine, fake: FakeEngine) -> None:
    await engine.call("entity_create", {"name": "A"})
    await engine.as_agent("stratus").call("entity_create", name="B")
    actors = [actor for tool, _, actor in fake.calls if tool == "entity_create"]
    assert actors == ["mcp:sky-agents", "mcp:sky-agents/stratus"]
    with pytest.raises(ToolError):
        await engine.call("entity_update", {"entity": "nope"}, check=True)


async def test_list_tools_and_specs(engine: AsyncEngine) -> None:
    specs = await engine.list_tools()
    by = {s.name: s for s in specs}
    assert by["entity_create"].mutates
    assert by["scene_overview"].read_only
    assert by["studio_message_send"].accepts("as")
    assert by["events_poll"].category == "agent"


async def test_typed_wrappers_validate_before_calling(engine: AsyncEngine, fake: FakeEngine) -> None:
    res = await engine.tools.viewport_capture(width=64, height=32)
    assert res.images and fake.calls[-1] == ("viewport_capture", {"width": 64, "height": 32}, "mcp:sky-agents")
    with pytest.raises(ValidationError):
        await engine.tools.viewport_capture(width="wide")  # type: ignore[arg-type]
    # Aliased keyword arguments: `as` is `as_` in Python.
    await engine.tools.studio_message_send(text="hello", as_="nimbus")
    assert fake.calls[-1][1] == {"text": "hello", "as": "nimbus"}


async def test_event_stream_long_polls_and_filters(engine: AsyncEngine) -> None:
    stream = engine.events(types=["studio.message"], wait_ms=5000)
    await stream.poll(wait_ms=0)  # pins the cursor at "now"

    async def later() -> None:
        await asyncio.sleep(0.05)
        await engine.call("entity_create", {"name": "noise"})
        await engine.as_agent("nimbus").call("studio_message_send", {"text": "@stratus widen the bridge"})

    task = asyncio.create_task(later())
    ev = await stream.next(timeout=5)
    await task
    assert ev is not None and ev.topic == "studio.message"
    assert ev.get("message")["text"] == "@stratus widen the bridge"
    assert await stream.next(timeout=0.1) is None
    await stream.close()


def test_sync_engine_facade(fake: FakeEngine) -> None:
    with Engine.fake(fake) as eng:
        res = eng.tools.entity_create(name="Sync")
        assert not res.is_error
        assert eng.as_agent("aurora").call("studio_presence", {"status": "working"}).data["agent"] == "aurora"
        assert {s.name for s in eng.list_tools()} >= {"scene_overview"}
        eng.call("studio_message_send", {"text": "hi"})
        events = eng.events(since=0, types=["studio.message"], wait_ms=0)
        assert next(events).topic == "studio.message"
        events.close()


def test_codegen_types_and_keywords() -> None:
    assert py_type({"type": "string", "enum": ["a", "b"]}) == "Literal['a', 'b']"
    assert py_type({"type": ["integer", "string"]}) == "int | str"
    assert py_type({"type": "array", "items": {"type": "number"}}) == "list[float]"
    src = generate([{"name": "demo_tool", "description": "Demo.", "inputSchema": {
        "type": "object", "properties": {"as": {"type": "string"}, "count": {"type": "integer"},
                                         "timeout": {"type": "number"}}, "required": ["count"]}}], "test")
    ns: dict[str, Any] = {"__name__": "skywalker_agents.engine._gen_test", "__package__": "skywalker_agents.engine"}
    exec(compile(src, "_generated", "exec"), ns)  # noqa: S102 - testing generated code
    calls: list[tuple[str, dict[str, Any]]] = []

    class Caller:
        def call(self, tool: str, args: dict[str, Any] | None = None, /, *, check: bool = False,
                 timeout: float | None = None) -> ToolResult:
            calls.append((tool, args or {}))
            return ToolResult.ok("ok")

    ns["Tools"](Caller()).demo_tool(count=2, as_="mira", timeout_=1.5)
    assert calls == [("demo_tool", {"as": "mira", "count": 2, "timeout": 1.5})]


async def test_json_rpc_connection_over_a_socket() -> None:
    tmp_path = Path(tempfile.mkdtemp(dir="/tmp"))  # unix socket paths are short
    path = str(tmp_path / "s.sock")

    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        while line := await reader.readline():
            msg = json.loads(line)
            if "id" not in msg:
                continue
            if msg["method"] == "initialize":
                result: Any = {"serverInfo": {"name": "stub"}, "instructions": "hi"}
            elif msg["method"] == "tools/call" and msg["params"]["name"] == "boom":
                writer.write((json.dumps({"jsonrpc": "2.0", "id": msg["id"],
                                          "error": {"code": -32602, "message": "no tool named 'boom'"}}) + "\n").encode())
                continue
            else:
                result = {"content": [{"type": "text", "text": msg["params"]["name"]}], "isError": False}
            writer.write((json.dumps({"jsonrpc": "2.0", "id": msg["id"], "result": result}) + "\n").encode())
            await writer.drain()
        writer.close()

    server = await asyncio.start_unix_server(handle, path)
    conn = await open_unix(path, "test")
    assert conn.server_info["name"] == "stub"
    results = await asyncio.gather(*(conn.call_tool(f"t{i}") for i in range(20)))  # pipelined
    assert [r.text for r in results] == [f"t{i}" for i in range(20)]
    with pytest.raises(ProtocolError):
        await conn.call_tool("boom")
    await conn.close()  # the server's handler sees EOF and returns
    server.close()
    await server.wait_closed()
    with pytest.raises(EngineConnectionError):
        await conn.call_tool("after")
    with pytest.raises(EngineConnectionError):
        await open_unix(str(tmp_path / "missing.sock"), "x")
    assert isinstance(conn, JsonRpcConnection)


async def test_tool_host_serves_python_tools_to_other_agents(engine: AsyncEngine, fake: FakeEngine) -> None:
    from skywalker_agents import ToolContext, tool

    seen: list[str] = []

    @tool
    async def shout(word: str, *, ctx: ToolContext) -> str:
        """Upper-case a word.

        Args:
            word: The word.
        """
        seen.append(ctx.agent_id)
        return word.upper()

    host = await engine.host_tools([shout], label="test")
    assert "py_shout" in {s["name"] for s in fake.list_tools()}
    res = await engine.as_agent("critic").call("py_shout", {"word": "lava"})
    assert res.text == "LAVA" and seen == ["critic"]
    await host.stop()
    assert "py_shout" not in fake.tools
    assert host.served == 1
