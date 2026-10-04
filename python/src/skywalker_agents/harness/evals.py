"""Evals: run a workflow (or any async callable) against scenarios and score the outcome with checks.

    scenarios = [Scenario("bridge", inputs={"goal": "fair bridge"}, setup=[{"tool": "scene_load", "args": {...}}],
                          checks=[metric("deaths", max=1), sim_trace(...), vision("The bridge is clearly visible")])]
    report = await run_eval(lambda ctx, s: wf.run(ctx.engine, inputs=s.inputs), scenarios, engine_factory)

Checks look at the engine after the run: tool results (``tool_check``), simulation traces
(``sim_trace_check``), playtest metrics (``metric_check``), captures judged by a vision model
(``vision_check``), or the run's own output (``text_check``). With a :class:`Replayer` the whole eval is
deterministic (no model calls).
"""

from __future__ import annotations

import json
import time
from collections.abc import Awaitable, Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any

from pydantic import BaseModel, Field

from .expr import evaluate

if TYPE_CHECKING:  # pragma: no cover
    from ..engine.client import AsyncEngine
    from ..llm.types import Provider


class CheckResult(BaseModel):
    name: str
    passed: bool
    score: float = 0.0
    detail: str = ""
    data: dict[str, Any] = Field(default_factory=dict)


@dataclass
class EvalContext:
    engine: AsyncEngine | None
    scenario: Scenario
    output: Any = None
    state: dict[str, Any] = field(default_factory=dict)


Check = Callable[[EvalContext], Awaitable[CheckResult]]


@dataclass
class Scenario:
    name: str
    inputs: dict[str, Any] = field(default_factory=dict)
    setup: list[dict[str, Any]] | Callable[[EvalContext], Awaitable[None]] = field(default_factory=list)
    checks: list[Check] = field(default_factory=list)
    weight: float = 1.0
    tags: list[str] = field(default_factory=list)


class ScenarioReport(BaseModel):
    scenario: str
    passed: bool
    score: float
    checks: list[CheckResult]
    seconds: float
    error: str = ""
    output: Any = None


class EvalReport(BaseModel):
    name: str
    passed: int
    total: int
    score: float
    scenarios: list[ScenarioReport]
    seconds: float

    def summary(self) -> str:
        lines = [f"{self.name}: {self.passed}/{self.total} scenarios passed, score {self.score:.2f}"]
        for s in self.scenarios:
            mark = "PASS" if s.passed else "FAIL"
            lines.append(f"  [{mark}] {s.scenario} ({s.score:.2f}, {s.seconds:.1f}s){' ' + s.error if s.error else ''}")
            for c in s.checks:
                lines.append(f"      {'ok ' if c.passed else 'no '} {c.name}: {c.detail}")
        return "\n".join(lines)


# ---------------------------------------------------------------------- checks
def tool_check(
    name: str,
    tool: str,
    args: dict[str, Any] | None = None,
    *,
    expect: str | None = None,
    predicate: Callable[[dict[str, Any], str], bool] | None = None,
) -> Check:
    """Calls ``tool`` and checks its result: ``expect`` is an expression over ``data`` and ``text``."""

    async def check(ctx: EvalContext) -> CheckResult:
        assert ctx.engine is not None, "tool checks need an engine"
        res = await ctx.engine.call(tool, args or {})
        if res.is_error:
            return CheckResult(name=name, passed=False, detail=res.text[:300])
        ok = True
        if expect is not None:
            ok = bool(evaluate(expect, {"data": res.data, "text": res.text}))
        if predicate is not None:
            ok = ok and predicate(res.data, res.text)
        return CheckResult(name=name, passed=ok, score=1.0 if ok else 0.0, detail=res.text[:200], data=res.data)

    return check


def sim_trace_check(
    name: str,
    *,
    entities: list[Any],
    properties: list[str],
    expect: str,
    ticks: int = 120,
    every: int = 10,
    hold: list[str] | None = None,
) -> Check:
    """Runs ``sim_trace`` and evaluates ``expect`` over its samples (``data``), e.g.
    ``"data.samples[-1].values['transform.position'][1] > 1"``."""
    args: dict[str, Any] = {"entities": entities, "properties": properties, "ticks": ticks, "every": every}
    if hold:
        args["hold"] = hold
    return tool_check(name, "sim_trace", args, expect=expect)


def metric_check(
    metric: str,
    *,
    min: float | None = None,
    max: float | None = None,
    playtest: dict[str, Any] | None = None,
    name: str = "",
) -> Check:
    """Plays the game (``playtest_run``) and checks one metric (completion_rate, deaths, est_fps, ...)."""

    async def check(ctx: EvalContext) -> CheckResult:
        assert ctx.engine is not None, "metric checks need an engine"
        report = ctx.state.get("playtest")
        if report is None:
            res = await ctx.engine.call(
                "playtest_run", {"include_heatmap": False, "runs": 2, "seconds": 30, **(playtest or {})}
            )
            if res.is_error:
                return CheckResult(name=name or metric, passed=False, detail=res.text[:300])
            report = ctx.state["playtest"] = res.data
        value = (report.get("metrics") or {}).get(metric)
        if value is None:
            return CheckResult(name=name or metric, passed=False, detail=f"no metric '{metric}' in the report")
        ok = (min is None or value >= min) and (max is None or value <= max)
        bounds = " ".join(
            x for x in (f">= {min}" if min is not None else "", f"<= {max}" if max is not None else "") if x
        )
        return CheckResult(
            name=name or metric,
            passed=ok,
            score=1.0 if ok else 0.0,
            detail=f"{metric}={value} {bounds}",
            data={"value": value, "playtest": report.get("id")},
        )

    return check


def vision_check(
    rubric: str, provider: Provider, *, model: str = "", capture: dict[str, Any] | None = None, name: str = "vision"
) -> Check:
    """Captures the viewport and asks a vision model whether the rubric holds (PASS/FAIL + a 0..1 score)."""
    from ..llm.types import ChatMessage, LLMRequest

    async def check(ctx: EvalContext) -> CheckResult:
        assert ctx.engine is not None, "vision checks need an engine"
        shot = await ctx.engine.call(
            "viewport_capture", {"width": 768, "height": 432, "annotate": False, **(capture or {})}
        )
        if shot.is_error or not shot.images:
            return CheckResult(name=name, passed=False, detail="no capture: " + shot.text[:200])
        req = LLMRequest(
            model=model or provider.default_model,
            max_tokens=2000,
            effort="low",
            system="You judge game screenshots strictly against a rubric. Answer with a JSON object "
            '{"pass": true|false, "score": 0..1, "why": "..."} and nothing else.',
            messages=[ChatMessage.user(f"Rubric: {rubric}", images=shot.images[:1])],
        )
        text = (await provider.complete(req)).text
        verdict = _json_in(text) or {}
        ok = bool(verdict.get("pass"))
        score = float(verdict.get("score", 1.0 if ok else 0.0))
        return CheckResult(name=name, passed=ok, score=score, detail=str(verdict.get("why", text))[:300])

    return check


def text_check(name: str, expect: str) -> Check:
    """An expression over the run's output (``output``), e.g. ``"output.status == 'completed'"``."""

    async def check(ctx: EvalContext) -> CheckResult:
        out = ctx.output.model_dump(mode="json") if isinstance(ctx.output, BaseModel) else ctx.output
        ok = bool(evaluate(expect, {"output": out}))
        return CheckResult(name=name, passed=ok, score=1.0 if ok else 0.0, detail=expect)

    return check


def _json_in(text: str) -> dict[str, Any] | None:
    start, end = text.find("{"), text.rfind("}")
    if start < 0 or end <= start:
        return None
    try:
        out = json.loads(text[start : end + 1])
        return out if isinstance(out, dict) else None
    except json.JSONDecodeError:
        return None


# ---------------------------------------------------------------------- runner
Subject = Callable[[EvalContext, Scenario], Awaitable[Any]]


async def run_eval(
    subject: Subject,
    scenarios: Sequence[Scenario],
    *,
    engine_factory: Callable[[], Awaitable[AsyncEngine]] | None = None,
    name: str = "eval",
    out: str | Path | None = None,
    pass_threshold: float = 1.0,
) -> EvalReport:
    """Runs ``subject`` once per scenario (a fresh engine each when ``engine_factory`` is given), then the checks."""
    t0 = time.monotonic()
    reports = []
    for sc in scenarios:
        s0 = time.monotonic()
        engine = await engine_factory() if engine_factory else None
        ctx = EvalContext(engine=engine, scenario=sc)
        error = ""
        try:
            if callable(sc.setup):
                await sc.setup(ctx)
            else:
                for st in sc.setup:
                    assert engine is not None, "setup tool calls need an engine"
                    await engine.call(st["tool"], st.get("args") or {}, check=True)
            ctx.output = await subject(ctx, sc)
            results = [await c(ctx) for c in sc.checks]
        except Exception as e:
            error = f"{type(e).__name__}: {e}"
            results = []
        finally:
            if engine is not None and engine_factory is not None:
                await engine.close()
        score = sum(r.score for r in results) / len(results) if results else 0.0
        passed = not error and bool(results) and all(r.passed for r in results) and score >= pass_threshold - 1e-9
        out_val = ctx.output.model_dump(mode="json") if isinstance(ctx.output, BaseModel) else ctx.output
        reports.append(
            ScenarioReport(
                scenario=sc.name,
                passed=passed,
                score=score,
                checks=results,
                seconds=time.monotonic() - s0,
                error=error,
                output=json.loads(json.dumps(out_val, default=str)),
            )
        )
    total_w = sum(s.weight for s in scenarios) or 1.0
    report = EvalReport(
        name=name,
        passed=sum(r.passed for r in reports),
        total=len(reports),
        score=sum(r.score * s.weight for r, s in zip(reports, scenarios, strict=True)) / total_w,
        scenarios=reports,
        seconds=time.monotonic() - t0,
    )
    if out:
        Path(out).parent.mkdir(parents=True, exist_ok=True)
        Path(out).write_text(report.model_dump_json(indent=2))
    return report
