"""An example skywalker-agents plugin.

Install it next to skywalker-agents (``pip install -e examples/custom_plugin``) and it contributes:

* a tool, ``hazard_audit``: hazards near the player's path with their spacing (agents get it with
  ``Agent(..., tools=[registry.tools.get("hazard_audit")])``, or serve it to every engine agent with
  ``engine.host_tools([...])`` as ``py_hazard_audit``);
* a hook, ``spend_limit``: refuses mutating tool calls after N per agent (a guardrail you configure);
* a workflow pattern, ``fairness_review``: audit, then a designer fixes and a critic signs off; usable from
  YAML as ``{id: review, pattern: fairness_review, args: {designer: mira, critic: vera}}``;
* a role template, ``hazard_designer``, for ``studio_agent_define``.

``sky-agents plugins`` lists them.
"""

from __future__ import annotations

import itertools
import re
from collections import defaultdict
from typing import Any

from skywalker_agents.engine.results import ToolResult
from skywalker_agents.hooks import Middleware, ToolInvocation, ToolNext
from skywalker_agents.tools import ToolContext, tool

_LINE = re.compile(r"^#(\d+) (.+?) \(.*?\bpos \[(-?[\d.]+)", re.M)


@tool(capabilities={"calls": ["scene_query"]})  # served to the engine, it may only call scene_query back
async def hazard_audit(min_gap: float = 6.0, *, ctx: ToolContext) -> dict[str, Any]:
    """List hazards along the level (by x) and flag pairs closer than ``min_gap`` meters.

    Args:
        min_gap: Hazards closer together than this (meters along x) are flagged as unfair clusters.
    """
    caller = ctx.session or ctx.engine
    if caller is None:
        return {"error": "hazard_audit needs an engine"}
    found = await caller.call("scene_query", {"tag": "hazard", "limit": 200})
    enemies = await caller.call("scene_query", {"tag": "enemy", "limit": 200})
    items = []
    for res, kind in ((found, "hazard"), (enemies, "enemy")):
        # scene_query answers one line per entity: "#101 Spikes 2 (cone #7c7f8c) pos [10.05, 0.6, 0] ..."
        for m in _LINE.finditer(res.text):
            items.append({"id": int(m.group(1)), "name": m.group(2), "kind": kind, "x": float(m.group(3))})
    items.sort(key=lambda i: i["x"])
    clusters = [
        {"a": a["name"], "b": b["name"], "gap": round(b["x"] - a["x"], 2)}
        for a, b in itertools.pairwise(items)
        if b["x"] - a["x"] < min_gap
    ]
    return {"count": len(items), "hazards": items, "too_close": clusters, "min_gap": min_gap}


class SpendLimit(Middleware):
    """Refuses an agent's mutating tool calls after ``max_edits`` (per agent, per process)."""

    def __init__(self, max_edits: int = 25) -> None:
        self.max_edits = max_edits
        self.edits: dict[str, int] = defaultdict(int)

    async def on_tool(self, inv: ToolInvocation, call_next: ToolNext) -> ToolResult:
        if inv.tool.mutates:
            self.edits[inv.agent_id] += 1
            if self.edits[inv.agent_id] > self.max_edits:
                return ToolResult.error(
                    "spend_limit",
                    f"@{inv.agent_id} used its {self.max_edits} edits",
                    "summarize what is left and hand it off",
                )
        return await call_next(inv)


async def fairness_review(ctx: Any, *, designer: str, critic: str, min_gap: float = 6.0) -> dict[str, Any]:
    """Audit hazards; the designer spreads out clusters; the critic reviews (a workflow pattern)."""
    audit = await hazard_audit.run({"min_gap": min_gap}, ToolContext(engine=ctx.engine))
    clusters = audit.data.get("too_close", [])
    if not clusters:
        return {"clusters": [], "fixed": False, "review": "nothing to fix"}
    fix = await ctx.agent(designer).run(
        f"These hazards are closer than {min_gap} m along the run: {clusters}. Spread them out (entity_update "
        "positions along x), keeping the level's length. Report what moved."
    )
    review = await ctx.agent(critic).run(f"Designer's report: {fix.text}\nIs the hazard spacing fair now?")
    return {"clusters": clusters, "fixed": True, "designer": fix.text, "review": review.text}


HAZARD_DESIGNER = {
    "role": "level_designer",
    "focus": "hazard placement and fairness",
    "focus_tags": ["hazards", "fairness"],
    "instructions": "Every hazard must be readable from 10 m and leave a safe landing.",
}


def register(registry: Any) -> None:
    registry.tools.add("hazard_audit", hazard_audit, origin="sky-agents-hazards")
    registry.hooks.add("spend_limit", SpendLimit, origin="sky-agents-hazards")
    registry.patterns.add("fairness_review", fairness_review, origin="sky-agents-hazards")
    registry.roles.add("hazard_designer", HAZARD_DESIGNER, origin="sky-agents-hazards")
