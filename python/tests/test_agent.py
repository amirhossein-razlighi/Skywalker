"""Agents against the fake engine and a scripted model: the loop, the studio integration and policies."""

from __future__ import annotations

from typing import Any

import pytest

from skywalker_agents import (
    Agent,
    AsyncEngine,
    AutoApprove,
    Budget,
    CacheMiddleware,
    FakeEngine,
    GuardrailMiddleware,
    Memory,
    MemorySink,
    Middleware,
    RedactionMiddleware,
    Role,
    ScriptedProvider,
    Tool,
    ToolContext,
    Tracer,
    tool,
)
from skywalker_agents.harness.approvals import CallbackApprover
from skywalker_agents.llm import LLMRequest, LLMResponse, TextBlock, ToolCall, Usage
from skywalker_agents.tools import Toolset, parse_docstring


def call(tool_name: str, /, **args: Any) -> dict[str, Any]:
    return {"name": tool_name, "input": args}


# ---------------------------------------------------------------------- tools
async def test_tool_decorator_schema_docstring_and_context() -> None:
    @tool(mutates=True)
    def place(name: str, height: float = 1.5, *, ctx: ToolContext) -> dict[str, Any]:
        """Place a marker.

        Args:
            name: What to call it.
            height: Meters above the ground,
                measured at the base.
        """
        return {"name": name, "height": height, "by": ctx.agent_id}

    assert place.mutates and place.name == "place"
    assert place.description == "Place a marker."
    props = place.input_schema["properties"]
    assert set(props) == {"name", "height"} and place.input_schema["required"] == ["name"]
    assert props["height"]["description"] == "Meters above the ground, measured at the base."
    res = await place.run({"name": "flag"}, ToolContext(agent_id="mira"))
    assert res.data == {"name": "flag", "height": 1.5, "by": "mira"}
    bad = await place.run({"height": "high"})
    assert bad.is_error and bad.error_code == "invalid_arguments"

    @tool
    async def explode() -> str:
        """Always fails."""
        raise RuntimeError("kaboom")

    out = await explode.run({})
    assert out.is_error and "kaboom" in out.text
    ts = Toolset([place, explode])
    assert ts.only(["place"]).names() == ["place"] and "explode" in ts
    with pytest.raises(ValueError, match="must match"):
        Tool("bad name", "", {}, explode._handler)
    assert parse_docstring("Summary.\n\nArgs:\n    a (int): first\n") == ("Summary.", {"a": "first"})


# ---------------------------------------------------------------------- agent loop
async def test_agent_runs_engine_tools_attributed_and_reports_to_the_studio(engine: AsyncEngine,
                                                                             fake: FakeEngine) -> None:
    provider = ScriptedProvider(agents={"stratus": [
        {"text": "Creating it.", "tool_calls": [call("entity_create", name="Bridge"), call("scene_overview")]},
        {"text": "Built the bridge."},
    ]})
    tracer = Tracer([sink := MemorySink()])
    agent = Agent("stratus", engine=engine, provider=provider, tracer=tracer)
    result = await agent.run("Build a bridge")
    assert result.ok and result.text == "Built the bridge." and result.turns == 2
    assert [c.name for c in result.tool_calls] == ["entity_create", "scene_overview"]
    actors = {actor for tool_name, _, actor in fake.calls if tool_name == "entity_create"}
    assert actors == {"mcp:sky-agents/stratus"}
    # Presence went working -> idle, usage was reported per model call.
    presences = [a["status"] for t, a, _ in fake.calls if t == "studio_presence"]
    assert presences == ["working", "idle"]
    assert fake.usage["stratus"]["requests"] == 2
    # Both tool results went back in one user message, after the assistant turn.
    second = provider.requests[1].messages
    assert [m.role for m in second] == ["user", "assistant", "user"]
    assert len(second[2].content) == 2
    names = [s.name for s in sink.spans]
    assert "invoke_agent stratus" in names and "execute_tool entity_create" in names
    assert any(n.startswith("chat ") for n in names)
    # The role came from the engine brief: system prompt and permitted tools.
    assert agent.role.on_roster and "Stratus" in provider.requests[0].system
    assert "events_poll" not in agent.toolset


async def test_unknown_tools_invalid_json_and_max_tokens(engine: AsyncEngine) -> None:
    def cut_off(req: LLMRequest) -> LLMResponse:
        return LLMResponse(content=[ToolCall(id="c9", name="entity_create", input={"na": "trunc"})],
                           stop_reason="max_tokens", usage=Usage(requests=1))

    def bad_json(req: LLMRequest) -> LLMResponse:
        return LLMResponse(content=[ToolCall(id="c1", name="entity_create", invalid_json=True, raw_input="{oops")],
                           stop_reason="tool_use", usage=Usage(requests=1))

    provider = ScriptedProvider([
        {"tool_calls": [call("does_not_exist")]}, bad_json, cut_off, {"text": "ok"}])
    agent = Agent(Role.adhoc("solo"), engine=engine, provider=provider, engine_tools=["entity_create"])
    result = await agent.run("go")
    assert result.ok and result.text == "ok"
    msgs = [m for m in provider.requests[-1].messages if m.role == "user"]
    texts = [b.text for m in msgs for b in m.content if hasattr(b, "tool_call_id")]
    assert "not available" in texts[0]
    assert "INVALID_JSON" in texts[1]
    assert "max_tokens" in texts[2]  # the cut-off call was never run


async def test_refusal_and_pause_turn(engine: AsyncEngine) -> None:
    def refused(req: LLMRequest) -> LLMResponse:
        return LLMResponse(stop_reason="refusal", refusal_category="cyber", usage=Usage(requests=1))

    agent = Agent(Role.adhoc("r"), engine=engine, provider=ScriptedProvider([refused]), engine_tools=False)
    res = await agent.run("x")
    assert res.stop_reason == "refusal" and "cyber" in res.error

    def paused(req: LLMRequest) -> LLMResponse:
        return LLMResponse(content=[TextBlock(text="working")], stop_reason="pause_turn", usage=Usage(requests=1))

    p = ScriptedProvider([paused, {"text": "done"}])
    res = await Agent(Role.adhoc("p"), engine=engine, provider=p, engine_tools=False).run("x")
    assert res.text == "done" and res.turns == 2
    assert p.requests[1].messages[-1].role == "assistant"  # resumed without a new user message


async def test_approvals_and_autonomy(engine: AsyncEngine, fake: FakeEngine) -> None:
    fake.agents["aurora"]["autonomy"] = "ask"
    script = [{"tool_calls": [call("entity_create", name="Lamp")]}, {"text": "asked"}]
    denied = await Agent("aurora", engine=engine, provider=ScriptedProvider(list(script))).run("add a lamp")
    assert denied.tool_calls[0].is_error and "declined" in denied.tool_calls[0].summary
    seen: list[str] = []
    approver = CallbackApprover(lambda req: seen.append(req.tool) or True)
    ok = await Agent("aurora", engine=engine, provider=ScriptedProvider(list(script)), approver=approver).run("x")
    assert seen == ["entity_create"] and not ok.tool_calls[0].is_error
    fake.agents["aurora"]["autonomy"] = "observe"
    observer = Agent("aurora", engine=engine, provider=ScriptedProvider([{"text": "looked"}]))
    await observer.setup()
    assert "entity_create" not in observer.toolset and "scene_overview" in observer.toolset


async def test_budgets_stop_the_loop(engine: AsyncEngine) -> None:
    looping = ScriptedProvider([{"tool_calls": [call("scene_overview")]} for _ in range(10)])
    res = await Agent(Role.adhoc("b"), engine=engine, provider=looping, budget=Budget(max_tool_calls=3),
                      engine_tools=["scene_overview"]).run("loop")
    assert res.stop_reason == "budget" and "tool_calls" in res.error
    res = await Agent(Role.adhoc("t"), engine=engine, provider=ScriptedProvider(
        [{"tool_calls": [call("scene_overview")]} for _ in range(10)]), max_turns=2,
        engine_tools=["scene_overview"]).run("loop")
    assert res.stop_reason == "max_turns"
    expensive = ScriptedProvider([{"tool_calls": [call("scene_overview")],
                                   "usage": {"input_tokens": 600, "output_tokens": 0}}] * 5)
    res = await Agent(Role.adhoc("c"), engine=engine, provider=expensive, budget=Budget(max_tokens=1000),
                      engine_tools=["scene_overview"]).run("loop")
    assert res.stop_reason == "budget"


async def test_middleware_redaction_guardrail_cache_and_custom(engine: AsyncEngine, fake: FakeEngine) -> None:
    order: list[str] = []

    class Spy(Middleware):
        async def on_tool(self, inv: Any, call_next: Any) -> Any:
            order.append(f"tool:{inv.tool.name}")
            return await call_next(inv)

        async def on_llm(self, request: Any, call_next: Any) -> Any:
            order.append("llm")
            return await call_next(request)

    provider = ScriptedProvider([
        {"tool_calls": [call("scene_overview"), call("scene_overview"), call("entity_create", name="X")]},
        {"tool_calls": [call("entity_update", entity="X")]},
        {"text": "done"},
    ])
    cache = CacheMiddleware()
    redact = RedactionMiddleware()
    agent = Agent(Role.adhoc("m"), engine=engine, provider=provider,
                  middleware=[Spy(), redact, GuardrailMiddleware(deny=["entity_update"]), cache],
                  engine_tools=["scene_overview", "entity_create", "entity_update"])
    res = await agent.run("my key is sk-ant-abcdefghijklmnopqrstuvwxyz, careful")
    assert res.ok
    assert "sk-ant-" not in provider.requests[0].messages[0].text and redact.redactions >= 1
    assert order[0] == "llm" and "tool:scene_overview" in order
    blocked = [c for c in res.tool_calls if c.name == "entity_update"][0]
    assert blocked.is_error and "guardrail" in blocked.summary
    assert sum(1 for t, *_ in fake.calls if t == "scene_overview") <= 2


async def test_memory_context_tools_and_episodes(engine: AsyncEngine) -> None:
    mem = Memory.open()
    await mem.for_agent("stratus").remember("The lava bridge must stay 3 m wide", kind="fact", scope="project")
    provider = ScriptedProvider(agents={"stratus": [
        {"tool_calls": [call("memory_remember", text="Players jump at x=12", kind="fact", scope="agent")]},
        {"text": "noted"}]})
    agent = Agent("stratus", engine=engine, provider=provider, memory=mem)
    res = await agent.run("How wide should the lava bridge be?")
    assert res.ok
    first_prompt = provider.requests[0].messages[0].text
    assert "3 m wide" in first_prompt and "Relevant memory" in first_prompt
    assert "memory_recall" in agent.toolset
    private = await mem.for_agent("stratus").recall("jump", scopes=["agent"])
    assert private and private[0].item.owner == "stratus"
    assert not await mem.for_agent("aurora").recall("jump", scopes=["agent"])
    episodes = await mem.for_agent("stratus").recall("", kinds=["episodic"])
    assert episodes and "lava bridge" in episodes[0].item.text


async def test_chat_keeps_history(engine: AsyncEngine) -> None:
    p = ScriptedProvider([{"text": "hello"}, {"text": "again"}])
    agent = Agent(Role.adhoc("c"), engine=engine, provider=p, engine_tools=False)
    await agent.chat("hi")
    await agent.chat("and?")
    assert [m.role for m in p.requests[1].messages] == ["user", "assistant", "user"]


async def test_roles_from_files(project: Any) -> None:
    (project / "agents" / "mira.agent.json").write_text(
        '{"format": "skywalker.agent", "version": 2, "id": "mira", "name": "Mira", "role": "level_designer",'
        ' "focus": "secret areas", "autonomy": "ask", "memory": ["likes ramps"]}')
    role = Role.load(project, "mira")
    assert role.default_model == "claude-opus-5-5" and role.autonomy == "ask"
    assert "secret areas" in role.local_system_prompt() and "likes ramps" in role.local_system_prompt()
    assert Role.adhoc("p", role="playtester").default_model == "claude-sonnet-5-5"
    with pytest.raises(FileNotFoundError, match="known: mira"):
        Role.load(project, "nobody")


def test_sync_agents_with_auto_approve(fake: FakeEngine) -> None:
    import asyncio

    async def go() -> Any:
        eng = AsyncEngine.fake(fake)
        a = Agent("aurora", engine=eng, provider=ScriptedProvider([{"text": "hi"}]), approver=AutoApprove())
        return await a.run("hello")

    assert asyncio.run(go()).text == "hi"
