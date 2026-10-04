"""Workflows as YAML/JSON, compatible with the engine's Studio loops.

```yaml
name: lighting_pass
budget: {max_cost_usd: 5, max_seconds: 1800}
defaults: {effort: high}              # Agent options for every agent step
inputs: {goal: "warm sunset over the canyon"}
steps:
  - id: build
    agent: aurora
    prompt: "Light the canyon: {{ inputs.goal }}"
  - id: shot
    tool: viewport_capture              # an engine tool call (args are templates too)
    args: {width: 768, height: 432}
    after: [build]
  - id: review
    agent: critic
    after: [shot]
    prompt: "Review the lighting. Start with APPROVED if it meets: {{ inputs.goal }}"
  - id: ship
    approval: "Keep this lighting?"     # a human gate (editor #approvals channel when attached)
    after: [review]
    when: "'APPROVED' in steps.review.text"
repeat: {max_iterations: 3, until: "'APPROVED' in steps.review.text"}
```

Other step kinds: ``pattern: director_critic`` (``args``), ``studio_loop: playtest_fix_verify`` (drives the
engine loop with Python agents; ``goal``, ``max_iterations``, ``define``), ``playtest: {...}``. ``map_over``
(an expression yielding a list) fans an agent step out; the item is ``{{ item }}``. A file that is an engine
loop definition (``stages``) runs as that Studio loop.
"""

from __future__ import annotations

import importlib
import json
from collections.abc import Sequence
from pathlib import Path
from typing import Any

import yaml

from ..errors import ApprovalDenied
from .approvals import ApprovalRequest, AutoDeny
from .budget import Budget
from .expr import evaluate, render, render_value
from .workflow import RunContext, Step, Workflow

_KINDS = ("agent", "tool", "pattern", "studio_loop", "approval", "playtest", "call")


def load_spec(source: str | Path | dict[str, Any]) -> dict[str, Any]:
    if isinstance(source, dict):
        return source
    text = Path(source).read_text()
    data = yaml.safe_load(text) if str(source).endswith((".yaml", ".yml")) else json.loads(text)
    if not isinstance(data, dict):
        raise ValueError(f"{source}: a workflow spec must be a mapping")
    data.setdefault("name", Path(source).stem.split(".")[0])
    return data


def load_workflow(source: str | Path | dict[str, Any]) -> tuple[Workflow, dict[str, Any]]:
    """A Workflow (and its spec) from YAML/JSON, an engine loop JSON, or ``module:attribute`` (Python)."""
    if isinstance(source, str) and ":" in source and not Path(source).exists() and not source.startswith("/"):
        mod_name, attr = source.split(":", 1)
        obj = getattr(importlib.import_module(mod_name), attr)
        wf = obj() if callable(obj) and not isinstance(obj, Workflow) else obj
        if not isinstance(wf, Workflow):
            raise TypeError(f"{source} is not a Workflow (or a function returning one)")
        return wf, {"name": wf.name}
    spec = load_spec(source)
    if "stages" in spec and "steps" not in spec:  # an engine Studio loop definition
        name = str(spec["name"])
        spec = {
            "name": name,
            "steps": [{"id": "loop", "studio_loop": name, "define": {k: v for k, v in spec.items() if k != "name"}}],
        }
    return build(spec), spec


def build(spec: dict[str, Any]) -> Workflow:
    budget = Budget.model_validate(spec["budget"]) if spec.get("budget") else None
    wf = Workflow(
        str(spec.get("name", "workflow")),
        budget=budget,
        concurrency=int(spec.get("concurrency", 4)),
        description=str(spec.get("description", "")),
    )
    defaults = dict(spec.get("defaults") or {})
    for raw in spec.get("steps") or []:
        wf.add(_step(raw, defaults))
    rep = spec.get("repeat")
    if rep:
        until_expr = rep.get("until")

        def until(ctx: RunContext) -> bool:
            return bool(evaluate(str(until_expr), ctx.namespace()))

        wf.repeat(until=until if until_expr else None, max_iterations=int(rep.get("max_iterations", 3)))
    return wf


def _step(raw: dict[str, Any], defaults: dict[str, Any]) -> Step:
    sid = str(raw.get("id") or "")
    if not sid:
        raise ValueError(f"every step needs an id: {raw}")
    kinds = [k for k in _KINDS if k in raw]
    if len(kinds) != 1:
        raise ValueError(f"step '{sid}' needs exactly one of {', '.join(_KINDS)} (got {kinds or 'none'})")
    kind = kinds[0]
    when_expr = raw.get("when")
    map_expr = raw.get("map_over")
    after = raw.get("after") or []
    if isinstance(after, str):
        after = [after]

    async def run(ctx: RunContext, item: Any = None) -> Any:
        ns = ctx.namespace()
        if map_expr is not None:
            ns["item"] = item
        if kind == "agent":
            options = {**defaults, **(raw.get("options") or {})}
            agent = ctx.agent(str(raw["agent"]), **options)
            result = await agent.run(render(str(raw.get("prompt", "")), ns))
            if result.stop_reason in ("error", "budget") and raw.get("fail_on_error", True):
                raise RuntimeError(f"agent {raw['agent']}: {result.error}")
            return result
        if kind == "tool":
            if ctx.engine is None:
                raise RuntimeError(f"step '{sid}': tool steps need an engine")
            res = await ctx.engine.call(str(raw["tool"]), render_value(raw.get("args") or {}, ns))
            if res.is_error and raw.get("fail_on_error", True):
                res.raise_for_error()
            return {"text": res.text, "data": res.data, "is_error": res.is_error, "images": len(res.images)}
        if kind == "playtest":
            if ctx.engine is None:
                raise RuntimeError(f"step '{sid}': playtests need an engine")
            args = {"include_heatmap": False, **render_value(raw.get("playtest") or {}, ns)}
            res = await ctx.engine.call("playtest_run", args, check=True)
            return {"text": res.text, "data": res.data}
        if kind == "pattern":
            from ..plugins.builtins import ensure_builtins
            from ..plugins.registry import registry

            ensure_builtins()
            fn = registry.patterns.get(str(raw["pattern"]))
            return await fn(ctx, **render_value(raw.get("args") or {}, ns))
        if kind == "studio_loop":
            from .studio_loop import run_studio_loop

            if ctx.engine is None:
                raise RuntimeError(f"step '{sid}': studio loops need an engine")
            return await run_studio_loop(
                ctx.engine,
                str(raw["studio_loop"]),
                ctx,
                goal=raw.get("goal"),
                max_iterations=raw.get("max_iterations"),
                define=raw.get("define"),
            )
        if kind == "approval":
            approver = ctx.approver or AutoDeny()
            decision = await approver.request(
                ApprovalRequest(agent="workflow", tool=f"step:{sid}", reason=render(str(raw["approval"]), ns))
            )
            if not decision.approved and raw.get("required", True):
                raise ApprovalDenied(f"step '{sid}' was not approved by {decision.by}: {decision.note}")
            return {"approved": decision.approved, "by": decision.by, "note": decision.note}
        if kind == "call":  # a Python callable "module:function" taking (ctx, **args)
            mod_name, attr = str(raw["call"]).split(":", 1)
            fn = getattr(importlib.import_module(mod_name), attr)
            return await fn(ctx, **render_value(raw.get("args") or {}, ns))
        raise AssertionError(kind)

    def when(ctx: RunContext) -> bool:
        return bool(evaluate(str(when_expr), ctx.namespace()))

    def map_over(ctx: RunContext) -> Sequence[Any]:
        val = evaluate(str(map_expr), ctx.namespace())
        return list(val or [])

    return Step(
        sid,
        run,
        list(after),
        when if when_expr else None,
        map_over if map_expr is not None else None,
        int(raw.get("concurrency", 4)),
        int(raw.get("retries", 0)),
        float(raw["timeout"]) if raw.get("timeout") else None,
        str(raw.get("description", "")),
    )
