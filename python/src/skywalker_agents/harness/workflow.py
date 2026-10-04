"""Workflows: steps with dependencies, conditions, fan-out, retries, timeouts, repetition, budgets,
checkpoints and resume.

    wf = Workflow("art_pass", budget=Budget(max_cost_usd=5))

    @wf.step()
    async def build(ctx):
        return await ctx.agent("aurora").run(f"Light the canyon: {ctx.inputs['goal']}")

    @wf.step(after=["build"])
    async def review(ctx):
        return await ctx.agent("critic").run("Review the canyon lighting; say APPROVED when it is good")

    wf.repeat(until=lambda ctx: "APPROVED" in ctx.text("review"), max_iterations=3)
    result = await wf.run(engine, inputs={"goal": "warm sunset"})

Every step result is checkpointed (``<project>/studio/runs/<run id>/checkpoint.json``), so a run
that stops (crash, budget, Ctrl-C) resumes where it left off with ``resume=<run id>``.
"""

from __future__ import annotations

import asyncio
import contextlib
import functools
import inspect
import json
import secrets
import time
from collections.abc import Awaitable, Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any, Literal

from pydantic import BaseModel, Field

from ..errors import BudgetExceeded
from ..observability import tracing as tr
from ..observability.cost import CostMeter
from ..observability.tracing import JsonlSink, Tracer
from .budget import Budget, BudgetTracker
from .checkpoint import Checkpoint, CheckpointStore, run_dir_for
from .retry import gather_limited

if TYPE_CHECKING:  # pragma: no cover
    from ..agents.agent import Agent
    from ..comms.blackboard import Blackboard
    from ..comms.bus import MessageBus
    from ..engine.client import AsyncEngine
    from ..hooks import Middleware
    from ..llm.types import Provider
    from ..memory.memory import Memory
    from .approvals import Approver

StepFn = Callable[..., Awaitable[Any]]
Predicate = Callable[["RunContext"], bool]
RunStatus = Literal["completed", "failed", "budget", "cancelled", "stopped"]


@dataclass
class Step:
    id: str
    fn: StepFn
    after: list[str] = field(default_factory=list)
    when: Predicate | None = None
    map_over: Callable[[RunContext], Sequence[Any]] | None = None
    concurrency: int = 4
    retries: int = 0
    timeout: float | None = None
    description: str = ""


class StepRecord(BaseModel):
    id: str
    status: Literal["done", "skipped", "failed"]
    result: Any = None
    error: str = ""
    seconds: float = 0.0
    iteration: int = 1


class RunResult(BaseModel):
    run_id: str
    workflow: str
    status: RunStatus
    iterations: int = 0
    results: dict[str, Any] = Field(default_factory=dict)
    history: list[dict[str, Any]] = Field(default_factory=list)
    state: dict[str, Any] = Field(default_factory=dict)
    error: str = ""
    cost: dict[str, Any] = Field(default_factory=dict)
    budget: dict[str, Any] = Field(default_factory=dict)
    seconds: float = 0.0
    run_dir: str = ""

    @property
    def ok(self) -> bool:
        return self.status == "completed"


class RunContext:
    """What a step sees: inputs, earlier results, the engine, agents, memory, messaging and limits."""

    def __init__(
        self,
        *,
        workflow: Workflow,
        run_id: str,
        engine: AsyncEngine | None,
        inputs: dict[str, Any],
        tracer: Tracer,
        budget: BudgetTracker,
        costs: CostMeter,
        provider: Provider | str | None,
        memory: Memory | None,
        approver: Approver | None,
        middleware: Sequence[Middleware],
        run_dir: Path,
        agent_defaults: dict[str, Any],
    ) -> None:
        self.workflow = workflow
        self.run_id = run_id
        self.engine = engine
        self.inputs = inputs
        self.tracer = tracer
        self.budget = budget
        self.costs = costs
        self.provider = provider
        self.memory = memory
        self.approver = approver
        self.middleware = list(middleware)
        self.run_dir = run_dir
        self.agent_defaults = dict(agent_defaults)
        self.iteration = 1
        self.results: dict[str, Any] = {}
        self.history: list[dict[str, Any]] = []
        self.state: dict[str, Any] = {}
        self._agents: dict[str, Agent] = {}
        self._bus: MessageBus | None = None
        self._board: Blackboard | None = None

    # ------------------------------------------------------------------ helpers for steps
    def agent(self, agent_id: str, **overrides: Any) -> Agent:
        """A run-configured agent (shared provider, memory, tracer, budget, approvals). Cached per id."""
        from ..agents.agent import Agent

        key = agent_id + (json.dumps(overrides, sort_keys=True, default=str) if overrides else "")
        if key not in self._agents:
            kwargs: dict[str, Any] = {
                "engine": self.engine,
                "provider": self.provider,
                "memory": self.memory,
                "tracer": self.tracer,
                "cost_meter": self.costs,
                "budget_tracker": self.budget,
                "approver": self.approver,
                "middleware": self.middleware,
                **self.agent_defaults,
            }
            kwargs.update(overrides)
            self._agents[key] = Agent(kwargs.pop("role", agent_id), **kwargs)
        return self._agents[key]

    @property
    def bus(self) -> MessageBus:
        if self._bus is None:
            from ..comms.bus import LocalBus, StudioBus

            self._bus = StudioBus(self.engine) if self.engine is not None else LocalBus()
        return self._bus

    @property
    def board(self) -> Blackboard:
        if self._board is None:
            from ..comms.blackboard import Blackboard

            if self.engine is None:
                raise RuntimeError("the blackboard needs an engine")
            self._board = Blackboard(self.engine)
        return self._board

    def result(self, step: str, default: Any = None) -> Any:
        return self.results.get(step, default)

    def text(self, step: str) -> str:
        """The text of a step's result (an agent result's final text, or str of anything else)."""
        r = self.results.get(step)
        if r is None:
            return ""
        if isinstance(r, dict):
            return str(r.get("text", json.dumps(r, default=str)))
        if isinstance(r, list):
            return "\n".join(str(x.get("text", x)) if isinstance(x, dict) else str(x) for x in r)
        return str(getattr(r, "text", r))

    def previous(self, step: str) -> Any:
        """The step's result in the previous iteration."""
        if not self.history:
            return None
        return self.history[-1].get(step)

    def namespace(self) -> dict[str, Any]:
        """For templates and expressions: inputs, steps, iteration, state, previous, run_id."""
        steps = {k: _as_dict(v) for k, v in self.results.items()}
        prev = {k: _as_dict(v) for k, v in (self.history[-1].items() if self.history else [])}
        return {
            "inputs": self.inputs,
            "steps": steps,
            "iteration": self.iteration,
            "state": self.state,
            "previous": prev,
            "run_id": self.run_id,
            "max_iterations": self.workflow.max_iterations,
        }


def _as_dict(v: Any) -> Any:
    if isinstance(v, BaseModel):
        return v.model_dump(mode="json")
    return v


def _jsonable(v: Any) -> Any:
    if isinstance(v, BaseModel):
        d = v.model_dump(mode="json")
        d.pop("messages", None)  # conversations are kept in the trace, not the checkpoint
        return d
    if isinstance(v, list):
        return [_jsonable(x) for x in v]
    if isinstance(v, dict):
        return {k: _jsonable(x) for k, x in v.items()}
    return json.loads(json.dumps(v, default=str))


class Workflow:
    """A graph of async steps, optionally repeated until a condition holds."""

    def __init__(self, name: str, *, budget: Budget | None = None, concurrency: int = 4, description: str = "") -> None:
        self.name = name
        self.budget = budget
        self.concurrency = concurrency
        self.description = description
        self.steps: dict[str, Step] = {}
        self.max_iterations = 1
        self.until: Predicate | None = None

    def step(
        self,
        id: str | None = None,
        *,
        after: Sequence[str] = (),
        when: Predicate | None = None,
        map_over: Callable[[RunContext], Sequence[Any]] | None = None,
        concurrency: int = 4,
        retries: int = 0,
        timeout: float | None = None,
    ) -> Callable[[StepFn], StepFn]:
        """Registers a step. ``map_over`` fans the step out over items (``fn(ctx, item)``, results in order);
        ``retries`` re-runs a failing step with backoff; ``timeout`` bounds each attempt (seconds)."""

        def deco(fn: StepFn) -> StepFn:
            self.add(
                Step(
                    id or fn.__name__,
                    fn,
                    list(after),
                    when,
                    map_over,
                    concurrency,
                    retries,
                    timeout,
                    inspect.getdoc(fn) or "",
                )
            )
            return fn

        return deco

    def add(self, step: Step) -> Workflow:
        if step.id in self.steps:
            raise ValueError(f"duplicate step '{step.id}'")
        for dep in step.after:
            if dep not in self.steps:
                raise ValueError(f"step '{step.id}' runs after unknown step '{dep}' (define it first)")
        self.steps[step.id] = step
        return self

    def repeat(self, *, until: Predicate | None = None, max_iterations: int = 3) -> Workflow:
        """Runs the whole graph again until ``until(ctx)`` holds (checked after each iteration)."""
        self.until = until
        self.max_iterations = max(1, max_iterations)
        return self

    # ------------------------------------------------------------------ running
    async def run(
        self,
        engine: AsyncEngine | None = None,
        *,
        inputs: dict[str, Any] | None = None,
        provider: Provider | str | None = None,
        memory: Memory | None = None,
        approver: Approver | None = None,
        middleware: Sequence[Middleware] = (),
        tracer: Tracer | None = None,
        run_id: str | None = None,
        resume: str | None = None,
        run_dir: str | Path | None = None,
        project: str | None = None,
        announce: bool = True,
        agent_defaults: dict[str, Any] | None = None,
        budget: Budget | None = None,
    ) -> RunResult:
        """Runs (or resumes) the workflow. Traces go to ``<run dir>/trace.jsonl``."""
        run_id = resume or run_id or time.strftime("%Y%m%d-%H%M%S-") + secrets.token_hex(2)
        base = Path(run_dir) if run_dir else run_dir_for(project or (engine.project if engine else None), run_id)
        store = CheckpointStore(base)
        ckpt = store.load() if resume else None
        tracer = tracer or Tracer()
        tracer.add_sink(JsonlSink(base / "trace.jsonl"))
        costs = CostMeter()
        tracker = BudgetTracker(budget or self.budget, scope=f"run:{self.name}")
        ctx = RunContext(
            workflow=self,
            run_id=run_id,
            engine=engine,
            inputs=dict(inputs or {}),
            tracer=tracer,
            budget=tracker,
            costs=costs,
            provider=provider,
            memory=memory,
            approver=approver,
            middleware=middleware,
            run_dir=base,
            agent_defaults=agent_defaults or {},
        )
        done_steps: dict[str, StepRecord] = {}
        if ckpt is not None:
            ctx.inputs = ckpt.inputs or ctx.inputs
            ctx.iteration = ckpt.iteration
            ctx.history = ckpt.history
            ctx.state = ckpt.state
            # Only finished steps are kept: failed and skipped ones run (or are re-evaluated) again.
            done_steps = {k: StepRecord.model_validate(v) for k, v in ckpt.steps.items() if v.get("status") == "done"}
            ctx.results = {k: r.result for k, r in done_steps.items() if r.status == "done"}
            tracker.tokens, tracker.cost_usd = ckpt.spent.get("tokens", 0), ckpt.spent.get("cost_usd", 0.0)
        started = time.monotonic()
        status: RunStatus = "completed"
        error = ""
        await self._announce(
            ctx, announce, f"Run {run_id} of workflow '{self.name}' " + ("resumed" if ckpt else "started")
        )
        with tracer.span(
            f"workflow {self.name}", "workflow", **{"skywalker.run_id": run_id, "skywalker.workflow": self.name}
        ) as span:
            try:
                while True:
                    with tracer.span(f"iteration {ctx.iteration}", "step", **{"skywalker.iteration": ctx.iteration}):
                        await self._run_iteration(ctx, done_steps, store)
                    if self.until is not None and self._safe_pred(self.until, ctx):
                        break
                    if ctx.iteration >= self.max_iterations:
                        break
                    ctx.history.append(dict(ctx.results))
                    ctx.results, done_steps = {}, {}
                    ctx.iteration += 1
                    self._save(store, ctx, done_steps, "running")
            except BudgetExceeded as e:
                status, error = "budget", str(e)
            except asyncio.CancelledError:
                status, error = "cancelled", "cancelled"
                self._save(store, ctx, done_steps, status)
                raise
            except StepFailed as e:
                status, error = "failed", str(e)
            except StopWorkflow as e:
                status, error = "stopped", str(e)
            finally:
                span.update(
                    {
                        "skywalker.status": status,
                        tr.COST: round(costs.total_usd, 6),
                        tr.INPUT_TOKENS: costs.total_usage.input_tokens,
                        tr.OUTPUT_TOKENS: costs.total_usage.output_tokens,
                    }
                )
                if error:
                    span.fail(error)
        self._save(store, ctx, done_steps, status)
        result = RunResult(
            run_id=run_id,
            workflow=self.name,
            status=status,
            iterations=ctx.iteration,
            results=_jsonable(ctx.results),
            history=_jsonable(ctx.history),
            state=_jsonable(ctx.state),
            error=error,
            cost=costs.report(),
            budget=tracker.snapshot(),
            seconds=time.monotonic() - started,
            run_dir=str(base),
        )
        (base / "report.json").write_text(result.model_dump_json(indent=2))
        await self._announce(
            ctx,
            announce,
            f"Run {run_id} of '{self.name}' {status} after {ctx.iteration} "
            f"iteration(s), ${costs.total_usd:.4f}" + (f": {error}" if error else ""),
        )
        if ctx._bus is not None:
            with contextlib.suppress(Exception):
                await ctx._bus.close()
        return result

    async def _run_iteration(self, ctx: RunContext, done: dict[str, StepRecord], store: CheckpointStore) -> None:
        pending = [s for s in self.steps.values() if s.id not in done]
        sem = asyncio.Semaphore(self.concurrency)
        finished: dict[str, asyncio.Event] = {s: asyncio.Event() for s in self.steps}
        for sid in done:
            finished[sid].set()

        async def run_one(step: Step) -> None:
            for dep in step.after:
                await finished[dep].wait()
            failed_deps = [d for d in step.after if d in done and done[d].status == "failed"]
            try:
                if failed_deps:
                    done[step.id] = StepRecord(
                        id=step.id, status="skipped", iteration=ctx.iteration, error=f"after failed {failed_deps}"
                    )
                elif step.when is not None and not self._safe_pred(step.when, ctx):
                    done[step.id] = StepRecord(id=step.id, status="skipped", iteration=ctx.iteration)
                else:
                    async with sem:
                        done[step.id] = await self._run_step(step, ctx)
                    if done[step.id].status == "done":
                        ctx.results[step.id] = done[step.id].result
                self._save(store, ctx, done, "running")
            finally:
                finished[step.id].set()

        results = await asyncio.gather(*(run_one(s) for s in pending), return_exceptions=True)
        for r in results:
            if isinstance(r, BaseException):
                raise r
        failures = [r for r in done.values() if r.status == "failed"]
        if failures:
            raise StepFailed("; ".join(f"{f.id}: {f.error}" for f in failures))

    async def _run_step(self, step: Step, ctx: RunContext) -> StepRecord:
        t0 = time.monotonic()
        attempts = step.retries + 1
        last = ""
        with ctx.tracer.span(
            f"step {step.id}", "step", **{"skywalker.step": step.id, "skywalker.iteration": ctx.iteration}
        ) as span:
            for attempt in range(1, attempts + 1):
                try:
                    ctx.budget.check()
                    value = await self._invoke(step, ctx)
                    return StepRecord(
                        id=step.id,
                        status="done",
                        result=_jsonable(value),
                        seconds=time.monotonic() - t0,
                        iteration=ctx.iteration,
                    )
                except (BudgetExceeded, StopWorkflow, asyncio.CancelledError):
                    raise
                except Exception as e:
                    last = f"{type(e).__name__}: {e}"
                    span.event("attempt_failed", attempt=attempt, error=last)
                    if attempt < attempts:
                        await asyncio.sleep(min(10.0, 0.5 * 2 ** (attempt - 1)))
            span.fail(last)
        return StepRecord(
            id=step.id, status="failed", error=last, seconds=time.monotonic() - t0, iteration=ctx.iteration
        )

    async def _invoke(self, step: Step, ctx: RunContext) -> Any:
        async def call(*args: Any) -> Any:
            coro = step.fn(ctx, *args)
            return await (asyncio.wait_for(coro, step.timeout) if step.timeout else coro)

        if step.map_over is None:
            return await call()
        items = list(step.map_over(ctx))
        out = await gather_limited([functools.partial(call, it) for it in items], step.concurrency)
        return list(out)

    @staticmethod
    def _safe_pred(pred: Predicate, ctx: RunContext) -> bool:
        try:
            return bool(pred(ctx))
        except Exception:
            return False

    def _save(self, store: CheckpointStore, ctx: RunContext, done: dict[str, StepRecord], status: str) -> None:
        store.save(
            Checkpoint(
                run_id=ctx.run_id,
                workflow=self.name,
                status=status,
                iteration=ctx.iteration,
                inputs=_jsonable(ctx.inputs),
                steps={k: v.model_dump(mode="json") for k, v in done.items()},
                history=_jsonable(ctx.history),
                state=_jsonable(ctx.state),
                spent={"tokens": ctx.budget.tokens, "cost_usd": ctx.budget.cost_usd},
            )
        )

    async def _announce(self, ctx: RunContext, enabled: bool, text: str) -> None:
        if not enabled or ctx.engine is None:
            return
        with contextlib.suppress(Exception):
            await ctx.engine.call(
                "studio_message_send",
                {
                    "channel": "runs",
                    "text": text,
                    "kind": "inform",
                    "data": {"run_id": ctx.run_id, "workflow": self.name},
                },
            )


class StepFailed(Exception):
    """A step failed after its retries."""


class StopWorkflow(Exception):
    """Raise from a step to end the run early (status ``stopped``)."""
