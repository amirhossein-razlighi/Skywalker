"""Against a real headless engine (``skywalker serve`` / ``skywalker mcp``). Skipped without a binary.

No API keys: models are scripted. Captures are tiny.
"""

from __future__ import annotations

import asyncio
import time
from collections.abc import AsyncIterator
from pathlib import Path

import pytest

from skywalker_agents import Agent, AsyncEngine, Engine, Memory, ScriptedProvider, StudioBus, run_studio_loop
from skywalker_agents.engine import codegen
from skywalker_agents.memory import SQLiteMemoryStore

pytestmark = pytest.mark.integration


@pytest.fixture
async def live(binary: str, sky_dash: Path) -> AsyncIterator[AsyncEngine]:
    eng = await AsyncEngine.spawn(sky_dash, binary=binary)
    await eng.call("studio_team_template", {"template": "indie_trio"}, check=True)
    yield eng
    await eng.close()


async def test_typed_tools_capture_and_generated_wrappers_match(live: AsyncEngine, binary: str) -> None:
    specs = {s.name for s in await live.list_tools()}
    assert {"events_poll", "tool_host_register", "studio_presence"} <= specs
    overview = await live.tools.scene_overview(check=True)
    assert "Sky Dash" in overview.text
    shot = await live.tools.viewport_capture(width=96, height=54, samples=1, annotate=False, check=True)
    assert shot.images and shot.pil_images()[0].size == (96, 54)
    tools, version = codegen.tools_from_binary(binary)
    committed = (Path(codegen.__file__).parent / "_generated.py").read_text()
    assert codegen.generate(tools, version) == committed, "run `sky-agents gen-tools` after changing engine tools"


async def test_event_stream_is_live_and_attributed(live: AsyncEngine) -> None:
    stream = live.events(types=["studio.message", "tool"], exclude_actors=["mcp:sky-agents/events"])
    await stream.poll(wait_ms=0)
    t0 = time.monotonic()

    async def later() -> None:
        await asyncio.sleep(0.2)
        await live.as_agent("stratus").call("studio_message_send", {"text": "@nimbus bridge widened"}, check=True)

    task = asyncio.create_task(later())
    ev = await stream.next(timeout=10)
    await task
    assert ev is not None and time.monotonic() - t0 < 5
    seen = [ev]
    while (e := await stream.next(timeout=1)) is not None:
        seen.append(e)
    msg = next(e for e in seen if e.topic == "studio.message")
    assert msg.get("message")["from"] == "stratus"
    assert any(e.type == "tool" and e.actor == "mcp:sky-agents/stratus" for e in seen)
    assert all(not str(e.tool).startswith("events_") for e in seen)  # polling is not in the activity feed
    await stream.close()


async def test_python_tools_served_to_every_agent(live: AsyncEngine) -> None:
    mem = Memory(SQLiteMemoryStore())
    host = await live.host_tools(mem.tools(), label="test memory")
    try:
        names = {s.name for s in await live.list_tools(refresh=True)}
        assert {"py_memory_remember", "py_memory_recall"} <= names
        r = await live.as_agent("aurora").call(
            "py_memory_remember",
            {"text": "Sunsets should stay warm orange", "kind": "fact", "scope": "agent"},
            check=True,
        )
        assert r.data["owner"] == "aurora"  # the caller's identity reached the Python tool
        mine = await live.as_agent("aurora").call("py_memory_recall", {"query": "sunset"}, check=True)
        theirs = await live.as_agent("stratus").call("py_memory_recall", {"query": "sunset"}, check=True)
        assert mine.data["hits"] and not theirs.data["hits"]
        listing = await live.call("tool_host_list", check=True)
        assert listing.data["hosts"][0]["served"] >= 3
    finally:
        await host.stop()
    names = {s.name for s in await live.list_tools(refresh=True)}
    assert "py_memory_recall" not in names


async def test_agent_edits_are_attributed_and_the_studio_sees_it(live: AsyncEngine) -> None:
    provider = ScriptedProvider(
        agents={
            "stratus": [
                {
                    "tool_calls": [
                        {
                            "name": "entity_create",
                            "input": {"name": "Agent Pillar", "mesh": "cube", "position": [2, 1, 0]},
                        }
                    ]
                },
                {"text": "Placed the pillar."},
            ]
        }
    )
    stream = live.events(types=["tool", "studio.agent", "studio.usage"])
    await stream.poll(wait_ms=0)
    result = await Agent("stratus", engine=live, provider=provider).run("Place a pillar")
    assert result.ok and result.tool_calls[0].is_error is False
    events = await stream.poll(wait_ms=500)
    while more := await stream.poll(wait_ms=200):
        events += more
    created = [e for e in events if e.tool == "entity_create"]
    assert created and created[0].actor == "mcp:sky-agents/stratus"
    statuses = [e.get("status") for e in events if e.action == "presence"]
    assert statuses[:1] == ["working"] and statuses[-1] == "idle"
    roster = await live.call("studio_agent_list", check=True)
    stratus = next(a for a in roster.data["agents"] if a["id"] == "stratus")
    assert "usage" in stratus or "tokens" in str(stratus)
    assert "Agent Pillar" in (await live.call("scene_overview")).text
    await stream.close()


async def test_agents_talk_through_studio_threads(live: AsyncEngine) -> None:
    bus = StudioBus(live)
    sub = bus.subscribe(agent="stratus")
    await bus.ready()

    async def stratus() -> None:
        env = await sub.get(10)
        assert env is not None and env.data == {"task": "bridge"}
        await bus.reply(env, "stratus", "Done: 3 m wide", data={"width": 3})

    task = asyncio.create_task(stratus())
    reply = await bus.request("nimbus", "stratus", "How wide is the bridge now?", data={"task": "bridge"}, timeout=15)
    await task
    assert reply is not None and reply.data == {"width": 3}
    thread = await bus.thread(reply.root)
    assert [m.sender for m in thread] == ["nimbus", "stratus"]
    await bus.close()


async def test_engine_studio_loop_driven_by_python_agents(live: AsyncEngine) -> None:
    provider = ScriptedProvider(
        agents={"aurora": [{"text": "Painted the sky"}], "nimbus": [{"text": "SIGNOFF the sky reads well"}]}
    )
    out = await run_studio_loop(
        live,
        "sky_pass",
        lambda aid: Agent(aid, engine=live, provider=provider, loop_member=True),
        define={
            "goal": "a readable sky",
            "stages": [
                {"id": "paint", "assignees": ["aurora"], "instruction": "Paint: {{goal}}"},
                {"id": "review", "assignees": ["nimbus"], "instruction": "Review {{inputs}}"},
            ],
            "stop": {"max_iterations": 2, "director_signoff": True},
        },
    )
    assert out["status"] == "done", out
    assert [r["report"] for r in out["reports"]] == ["Painted the sky", "SIGNOFF the sky reads well"]
    status = await live.call("studio_loop_status", {"loop": "sky_pass"}, check=True)
    assert status.data["status"] == "done"


async def test_stdio_transport(binary: str, sky_dash: Path) -> None:
    eng = await AsyncEngine.spawn(sky_dash, binary=binary, transport="stdio")
    try:
        assert eng.mode == "stdio" and not eng.supports_sessions
        await eng.call("studio_team_template", {"template": "indie_trio"}, check=True)
        # One connection: identity travels as `as` on studio calls.
        await eng.as_agent("nimbus").call("studio_message_send", {"text": "hello over stdio"}, check=True)
        stream = eng.events(since=0, types=["studio.message"])
        ev = await stream.next(timeout=5)
        assert ev is not None and ev.get("message")["from"] == "nimbus"
    finally:
        await eng.close()


def test_sync_client_spawn(binary: str, sky_dash: Path) -> None:
    with Engine.spawn(sky_dash, binary=binary) as eng:
        assert "Sky Dash" in eng.tools.scene_overview().text
