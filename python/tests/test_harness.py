from __future__ import annotations

import asyncio
import json
from pathlib import Path
from typing import Any

import pytest

from skywalker_agents import (
    AsyncEngine,
    AutoApprove,
    AutoDeny,
    Budget,
    FakeEngine,
    Recorder,
    Replayer,
    RunContext,
    ScriptedProvider,
    Workflow,
    load_workflow,
    run_studio_loop,
)
from skywalker_agents.errors import ReplayMismatch
from skywalker_agents.harness import (
    Scenario,
    debate,
    director_critic,
    evaluate,
    list_runs,
    map_reduce,
    metric_check,
    plan_and_execute,
    playtest_triage_fix_verify,
    render,
    retry_async,
    run_eval,
    text_check,
    tool_check,
)
from skywalker_agents.harness.expr import ExpressionError
from skywalker_agents.observability import load_trace
from skywalker_agents.observability.viewer import render_stats, render_tree


async def test_graph_dependencies_parallelism_conditions_and_fan_out(tmp_path: Path) -> None:
    wf = Workflow("demo")
    order: list[str] = []

    @wf.step()
    async def a(ctx: RunContext) -> int:
        order.append("a")
        await asyncio.sleep(0.02)
        return 1

    @wf.step()
    async def b(ctx: RunContext) -> int:
        order.append("b")
        return 2

    @wf.step(after=["a", "b"])
    async def total(ctx: RunContext) -> int:
        return int(ctx.result("a")) + int(ctx.result("b"))

    @wf.step(after=["total"], map_over=lambda ctx: [1, 2, 3])
    async def squares(ctx: RunContext, item: int) -> int:
        return item * item

    @wf.step(after=["total"], when=lambda ctx: ctx.result("total") > 100)
    async def never(ctx: RunContext) -> str:
        return "no"

    res = await wf.run(run_dir=tmp_path / "r")
    assert res.ok and res.results == {"a": 1, "b": 2, "total": 3, "squares": [1, 4, 9]}
    assert order[:2] == ["a", "b"]  # both started before either finished
    ckpt = json.loads((tmp_path / "r" / "checkpoint.json").read_text())
    assert ckpt["steps"]["never"]["status"] == "skipped"
    spans = load_trace(tmp_path / "r" / "trace.jsonl")
    assert {s["name"] for s in spans} >= {"workflow demo", "step total"}
    assert "workflow demo" in render_tree(spans)


async def test_retries_failures_and_resume(tmp_path: Path) -> None:
    attempts = {"flaky": 0, "boom": 0}

    def build() -> Workflow:
        wf = Workflow("resume")

        @wf.step(retries=2)
        async def flaky(ctx: RunContext) -> str:
            attempts["flaky"] += 1
            if attempts["flaky"] < 2:
                raise RuntimeError("transient")
            return "ok"

        @wf.step(after=["flaky"])
        async def boom(ctx: RunContext) -> str:
            attempts["boom"] += 1
            if not ctx.state.get("fixed"):
                ctx.state["fixed"] = True
                raise RuntimeError("needs a fix")
            return "fixed"

        return wf

    first = await build().run(run_dir=tmp_path / "r", run_id="r1")
    assert first.status == "failed" and "needs a fix" in first.error and attempts["flaky"] == 2
    second = await build().run(run_dir=tmp_path / "r", resume="r1")
    assert second.ok and second.results == {"flaky": "ok", "boom": "fixed"}
    assert attempts["flaky"] == 2  # not re-run: it was checkpointed


async def test_repeat_until_and_budget(tmp_path: Path) -> None:
    wf = Workflow("loop")

    @wf.step()
    async def count(ctx: RunContext) -> int:
        return ctx.iteration

    wf.repeat(until=lambda ctx: ctx.result("count") >= 3, max_iterations=10)
    res = await wf.run(run_dir=tmp_path / "a")
    assert res.iterations == 3 and [h["count"] for h in res.history] == [1, 2]

    slow = Workflow("slow", budget=Budget(max_seconds=0.05))

    @slow.step()
    async def wait(ctx: RunContext) -> None:
        await asyncio.sleep(0.03)

    slow.repeat(max_iterations=20)
    out = await slow.run(run_dir=tmp_path / "b")
    assert out.status == "budget"


def test_expressions_and_templates() -> None:
    ns = {"steps": {"review": {"text": "APPROVED, nice"}}, "iteration": 2, "inputs": {"n": [1, 2]}}
    assert evaluate("'APPROVED' in steps.review.text and iteration < 3", ns)
    assert evaluate("len(inputs.n) == 2", ns)
    assert evaluate("steps.missing.text", ns) is None
    assert render("it {{ iteration }}: {{ inputs.n }}", ns) == "it 2: [1, 2]"
    for bad in ("__import__('os')", "steps.__class__", "(lambda: 1)()", "open('x')"):
        with pytest.raises(ExpressionError):
            evaluate(bad, ns)


async def test_declarative_workflow_with_agents_tools_approval_and_patterns(
    engine: AsyncEngine, tmp_path: Path
) -> None:
    spec = {
        "name": "lighting",
        "inputs": {"goal": "warm sunset"},
        "steps": [
            {"id": "build", "agent": "aurora", "prompt": "Light it: {{ inputs.goal }}"},
            {"id": "shot", "tool": "viewport_capture", "args": {"width": 32, "height": 16}, "after": ["build"]},
            {"id": "review", "agent": "critic", "after": ["shot"], "prompt": "Review: {{ steps.build.text }}"},
            {"id": "ship", "approval": "Keep it?", "after": ["review"], "when": "'APPROVED' in steps.review.text"},
            {
                "id": "notes",
                "agent": "aurora",
                "after": ["review"],
                "map_over": "['sky', 'sun']",
                "prompt": "Note about {{ item }}",
            },
        ],
        "repeat": {"max_iterations": 2, "until": "'APPROVED' in steps.review.text"},
    }
    wf, _ = load_workflow(spec)
    provider = ScriptedProvider(
        agents={
            "aurora": [
                {"text": "lit v1"},
                {"text": "sky note"},
                {"text": "sun note"},
                {"text": "lit v2"},
                {"text": "sky 2"},
                {"text": "sun 2"},
            ],
            "critic": [{"text": "Too cold."}, {"text": "APPROVED"}],
        }
    )
    res = await wf.run(
        engine, inputs=spec["inputs"], provider=provider, approver=AutoApprove(), run_dir=tmp_path / "run"
    )
    assert res.ok, res.error
    assert res.iterations == 2 and res.results["ship"]["approved"] is True
    assert res.results["shot"]["images"] == 1
    assert [r["text"] for r in res.results["notes"]] == ["sky 2", "sun 2"]
    assert "lit v1" in provider.requests[1].messages[0].text or any(
        "lit v1" in r.messages[0].text for r in provider.requests
    )
    denied = await load_workflow({"name": "gate", "steps": [{"id": "g", "approval": "ok?"}]})[0].run(
        engine, approver=AutoDeny(), run_dir=tmp_path / "gate"
    )
    assert denied.status == "failed" and "not approved" in denied.error


async def test_engine_loop_json_runs_as_a_studio_loop(engine: AsyncEngine, tmp_path: Path) -> None:
    path = tmp_path / "art.loop.json"
    path.write_text(
        json.dumps(
            {
                "name": "art",
                "goal": "warm canyon",
                "stop": {"max_iterations": 2},
                "stages": [
                    {"id": "build", "assignees": ["aurora"], "instruction": "Work on {{goal}}"},
                    {"id": "review", "assignees": ["critic"], "instruction": "Review {{inputs}}"},
                ],
            }
        )
    )
    wf, _ = load_workflow(path)
    provider = ScriptedProvider(
        agents={
            "aurora": [{"text": "v1"}, {"text": "v2"}],
            "critic": [{"text": "more red"}, {"text": "SIGNOFF looks great"}],
        }
    )
    res = await wf.run(engine, provider=provider, run_dir=tmp_path / "r")
    assert res.ok, res.error
    loop = res.results["loop"]
    assert loop["status"] == "done"
    assert [r["report"] for r in loop["reports"]] == ["v1", "more red", "v2", "SIGNOFF looks great"]


async def test_studio_loop_driver_parallel_and_approval(fake: FakeEngine, engine: AsyncEngine) -> None:
    await engine.call(
        "studio_loop_define",
        {
            "name": "duo",
            "stages": [{"id": "both", "assignees": ["aurora", "stratus"], "instruction": "go"}],
            "stop": {"max_iterations": 1},
        },
    )
    provider = ScriptedProvider([{"text": "a"}, {"text": "b"}])
    out = await run_studio_loop(
        engine,
        "duo",
        lambda aid: __import__("skywalker_agents").Agent(aid, engine=engine, provider=provider, loop_member=True),
    )
    assert out["status"] == "done" and sorted(r["report"] for r in out["reports"]) == ["a", "b"]


async def test_record_and_replay_are_deterministic(engine: AsyncEngine, fake: FakeEngine, tmp_path: Path) -> None:
    def build() -> Workflow:
        wf = Workflow("rr")

        @wf.step()
        async def work(ctx: RunContext) -> Any:
            return await ctx.agent("stratus").run("make a pillar")

        return wf

    script = {
        "stratus": [{"tool_calls": [{"name": "entity_create", "input": {"name": "Pillar"}}]}, {"text": "made it"}]
    }
    cassette = tmp_path / "golden.jsonl"
    live = await build().run(
        engine,
        provider=ScriptedProvider(agents=script),
        middleware=[Recorder(cassette)],
        run_dir=tmp_path / "live",
        announce=False,
    )
    created = len([c for c in fake.calls if c[0] == "entity_create"])
    rep = Replayer(cassette)
    again = await build().run(
        engine, provider=rep.provider(), middleware=[rep], run_dir=tmp_path / "replay", announce=False
    )
    assert again.results["work"]["text"] == live.results["work"]["text"] == "made it"
    assert len([c for c in fake.calls if c[0] == "entity_create"]) == created  # tools were replayed, not re-run
    assert rep.served >= 3 and not rep.misses
    strict = Replayer(cassette)

    wf = Workflow("other")

    @wf.step()
    async def other(ctx: RunContext) -> Any:
        return await ctx.agent("stratus").run("something else entirely")

    res = await wf.run(engine, provider=strict.provider(), middleware=[strict], run_dir=tmp_path / "x")
    assert res.status == "failed" and "ReplayMismatch" in res.error
    assert ReplayMismatch.__name__ in res.error


async def test_patterns(engine: AsyncEngine, fake: FakeEngine, tmp_path: Path) -> None:
    wf = Workflow("patterns")
    out: dict[str, Any] = {}

    @wf.step()
    async def run_all(ctx: RunContext) -> None:
        out["dc"] = await director_critic(ctx, maker="aurora", critic="critic", task="warm canyon", rounds=3)
        out["debate"] = await debate(
            ctx, agents=["nimbus", "stratus"], question="Jump height?", rounds=1, judge="critic"
        )
        out["mr"] = await map_reduce(
            ctx, items=["a", "b"], mapper="stratus", reducer="nimbus", prompt="Describe {item}"
        )
        out["plan"] = await plan_and_execute(ctx, planner="nimbus", executors=["stratus"], goal="add coins")

    provider = ScriptedProvider(
        agents={
            "aurora": [{"text": "v1"}, {"text": "v2"}],
            "critic": [{"text": "too cold"}, {"text": "APPROVED"}, {"text": "Decision: 2 m"}],
            "nimbus": [
                {"text": "2 m"},
                {"text": "combined"},
                {"text": '```json\n[{"title": "Place coins", "assignee": "stratus", "instructions": "5 coins"}]```'},
                {"text": "DONE"},
            ],
            "stratus": [{"text": "3 m"}, {"text": "A!"}, {"text": "B!"}, {"text": "placed"}],
        }
    )
    res = await wf.run(engine, provider=provider, run_dir=tmp_path / "p")
    assert res.ok, res.error
    assert out["dc"]["approved"] and len(out["dc"]["rounds"]) == 2
    assert out["debate"]["verdict"] == "Decision: 2 m"
    assert out["mr"] == {"results": ["A!", "B!"], "reduced": "combined"}
    assert out["plan"]["review"] == "DONE" and fake.tasks[-1]["status"] == "done"


async def test_playtest_triage_fix_verify(engine: AsyncEngine, fake: FakeEngine, tmp_path: Path) -> None:
    wf = Workflow("ptfv")

    @wf.step()
    async def loop(ctx: RunContext) -> Any:
        return await playtest_triage_fix_verify(
            ctx, director="nimbus", fixers=["stratus"], targets={"deaths": {"max": 1}}, iterations=3
        )

    provider = ScriptedProvider(
        agents={
            "nimbus": [
                {
                    "tool_calls": [
                        {
                            "name": "studio_decide",
                            "input": {
                                "feedback": "F-1",
                                "verdict": "act",
                                "rationale": "unfair",
                                "tasks": [{"title": "Widen the bridge", "assignee": "stratus"}],
                            },
                        }
                    ]
                },
                {"text": "triaged"},
            ],
            "stratus": [
                {"tool_calls": [{"name": "studio_task_update", "input": {"task": "T-1", "status": "done"}}]},
                {"text": "widened"},
            ],
        }
    )
    res = await wf.run(engine, provider=provider, run_dir=tmp_path / "x")
    assert res.ok, res.error
    final = res.results["loop"]
    assert final["met"] and fake.feedback[0]["status"] == "fixed"


async def test_evals(engine: AsyncEngine, tmp_path: Path) -> None:
    async def subject(ctx: Any, sc: Scenario) -> dict[str, Any]:
        await ctx.engine.call("entity_create", {"name": sc.inputs["name"]})
        return {"status": "completed"}

    scenarios = [
        Scenario(
            "pillar",
            inputs={"name": "Pillar"},
            checks=[
                tool_check("exists", "scene_overview", expect="'Pillar' in text"),
                text_check("completed", "output.status == 'completed'"),
                metric_check("deaths", max=5),
            ],
        )
    ]
    report = await run_eval(subject, scenarios, engine_factory=lambda: _fake_engine(), out=tmp_path / "r.json")
    assert report.passed == 1 and report.score == 1.0, report.summary()
    assert (tmp_path / "r.json").exists()


async def _fake_engine() -> AsyncEngine:
    return AsyncEngine.fake(FakeEngine())


async def test_retry_backoff() -> None:
    from skywalker_agents.errors import ProviderError

    calls = {"n": 0}

    async def flaky() -> str:
        calls["n"] += 1
        if calls["n"] < 3:
            raise ProviderError("503")
        return "ok"

    assert await retry_async(flaky, attempts=3, base_delay=0.001) == "ok"

    async def fatal() -> str:
        calls["n"] += 1
        raise ProviderError("bad request", retryable=False)

    calls["n"] = 0
    with pytest.raises(ProviderError):
        await retry_async(fatal, attempts=5, base_delay=0.001)
    assert calls["n"] == 1


def test_runs_listing_and_stats(tmp_path: Path) -> None:
    run = tmp_path / "studio" / "runs" / "r1"
    run.mkdir(parents=True)
    (run / "report.json").write_text(json.dumps({"workflow": "w", "status": "completed", "cost": {"total_usd": 0.5}}))
    (run / "trace.jsonl").write_text(
        json.dumps(
            {
                "name": "chat m",
                "kind": "llm",
                "span_id": "1",
                "parent_id": None,
                "duration": 1.0,
                "attributes": {"gen_ai.agent.id": "mira", "gen_ai.usage.input_tokens": 10, "skywalker.cost_usd": 0.01},
            }
        )
        + "\n"
    )
    runs = list_runs(tmp_path)
    assert runs[0]["run_id"] == "r1" and runs[0]["cost_usd"] == 0.5
    assert "mira" in render_stats(load_trace(run / "trace.jsonl"))
