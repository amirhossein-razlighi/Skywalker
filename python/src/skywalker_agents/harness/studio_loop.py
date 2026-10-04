"""Driving the engine's Studio loops with Python agents.

The loop stays a state machine in the engine (``studio_loop_start`` / ``studio_loop_advance``): it
runs playtests and verification itself, measures effects, and hands out assignments. This driver
executes each assignment with a Python :class:`Agent` (in parallel when the stage allows), reports
back, and answers human approval gates through an :class:`Approver` (the editor, by default when
attached). The editor's Loops view shows the run live, exactly as if the crew ran it.
"""

from __future__ import annotations

import functools
from collections.abc import Callable
from typing import TYPE_CHECKING, Any

from ..errors import ToolError
from .approvals import ApprovalRequest, Approver, AutoDeny
from .retry import gather_limited

if TYPE_CHECKING:  # pragma: no cover
    from ..agents.agent import Agent
    from ..engine.client import AsyncEngine
    from .workflow import RunContext

AgentFactory = Callable[[str], "Agent"]


async def run_studio_loop(
    engine: AsyncEngine,
    loop: str,
    agent_for: AgentFactory | RunContext,
    *,
    goal: str | None = None,
    max_iterations: int | None = None,
    define: dict[str, Any] | None = None,
    approver: Approver | None = None,
    concurrency: int = 4,
    max_advances: int = 200,
) -> dict[str, Any]:
    """Runs ``loop`` to the end; returns the final ``studio_loop_status`` payload (with ``reports``)."""
    factory: AgentFactory
    if callable(agent_for):
        factory = agent_for
    else:
        ctx = agent_for

        def factory(agent_id: str) -> Agent:
            return ctx.agent(agent_id, loop_member=True)

        approver = approver or ctx.approver
    if define is not None:
        await engine.call("studio_loop_define", {"name": loop, **define}, check=True)
    start: dict[str, Any] = {"loop": loop}
    if goal:
        start["goal"] = goal
    if max_iterations:
        start["max_iterations"] = max_iterations
    status = (await engine.call("studio_loop_start", start, check=True)).data
    reports_log: list[dict[str, Any]] = []
    for _ in range(max_advances):
        state = status.get("status")
        if state in ("done", "stopped", "idle"):
            break
        if state == "awaiting_approval":
            decision = await (approver or AutoDeny()).request(
                ApprovalRequest(
                    agent="",
                    tool="studio_loop_advance",
                    args={"loop": loop},
                    reason=f"loop {loop} needs approval to "
                    f"continue (stage {status.get('stage')}, iteration {status.get('iteration')})",
                )
            )
            status = (
                await engine.call("studio_loop_advance", {"loop": loop, "approve": decision.approved}, check=True)
            ).data
            continue
        assignments = list(status.get("assignments") or [])
        if not assignments:
            status = (await engine.call("studio_loop_advance", {"loop": loop, "complete_stage": True}, check=True)).data
            continue

        async def work(a: dict[str, Any]) -> dict[str, Any]:
            agent = factory(str(a["agent"]))
            result = await agent.run(str(a.get("prompt", "")))
            text = result.text or f"(no report: {result.stop_reason} {result.error})"
            return {"agent": a["agent"], "report": text}

        limit = concurrency if status.get("parallel", True) else 1
        reports = await gather_limited([functools.partial(work, a) for a in assignments], limit)
        clean = [r for r in reports if isinstance(r, dict)]
        reports_log += [{**r, "stage": status.get("stage"), "iteration": status.get("iteration")} for r in clean]
        res = await engine.call("studio_loop_advance", {"loop": loop, "reports": clean})
        if res.is_error:
            raise ToolError("studio_loop_advance", res.error_code, res.error_message, res.error_hint)
        status = res.data
    return {**status, "reports": reports_log}
