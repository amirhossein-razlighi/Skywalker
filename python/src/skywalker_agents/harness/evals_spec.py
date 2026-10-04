"""Declarative eval specs (``sky-agents eval spec.yaml``).

```yaml
name: bridge_fairness
workflow: workflows/fix_bridge.yaml          # what is evaluated (any `sky-agents run` workflow)
scenarios:
  - name: default
    inputs: {goal: "deaths <= 1"}
    setup:
      - {tool: scene_load, args: {path: scenes/main.sky.json}}
    checks:
      - {type: metric, metric: deaths, max: 1, playtest: {runs: 2, seconds: 20}}
      - {type: tool, name: bridge exists, tool: scene_query, args: {name: "Bridge*"}, expect: "len(data.entities) > 0"}
      - {type: text, name: completed, expect: "output.status == 'completed'"}
```
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from .declarative import load_spec, load_workflow
from .evals import (
    Check,
    EvalContext,
    Scenario,
    Subject,
    metric_check,
    sim_trace_check,
    text_check,
    tool_check,
    vision_check,
)


def _check(spec: dict[str, Any], provider: Any) -> Check:
    kind = spec.get("type")
    name = str(spec.get("name") or kind)
    if kind == "metric":
        return metric_check(
            str(spec["metric"]), min=spec.get("min"), max=spec.get("max"), playtest=spec.get("playtest"), name=name
        )
    if kind == "tool":
        return tool_check(name, str(spec["tool"]), spec.get("args"), expect=spec.get("expect"))
    if kind == "sim_trace":
        return sim_trace_check(
            name,
            entities=list(spec["entities"]),
            properties=list(spec["properties"]),
            expect=str(spec["expect"]),
            ticks=int(spec.get("ticks", 120)),
            every=int(spec.get("every", 10)),
            hold=spec.get("hold"),
        )
    if kind == "text":
        return text_check(name, str(spec["expect"]))
    if kind == "vision":
        if provider is None:
            from ..llm import get_provider

            provider = get_provider(str(spec.get("provider", "anthropic")))
        return vision_check(
            str(spec["rubric"]), provider, model=str(spec.get("model", "")), capture=spec.get("capture"), name=name
        )
    raise ValueError(f"unknown check type '{kind}' (metric, tool, sim_trace, text, vision)")


def load_eval(
    path: str | Path, *, provider: Any = None, project: str | None = None
) -> tuple[Subject, list[Scenario], str]:
    spec = load_spec(path)
    base = Path(path).parent
    wf_ref = spec.get("workflow")
    if wf_ref is None:
        raise ValueError("an eval spec needs `workflow`")
    wf_path = base / str(wf_ref) if (base / str(wf_ref)).exists() else wf_ref
    workflow, wf_spec = load_workflow(wf_path if isinstance(wf_path, (str, Path)) else str(wf_path))
    scenarios = [
        Scenario(
            name=str(s.get("name", f"scenario {i + 1}")),
            inputs=dict(s.get("inputs") or {}),
            setup=list(s.get("setup") or []),
            checks=[_check(c, provider) for c in s.get("checks", [])],
            weight=float(s.get("weight", 1.0)),
            tags=list(s.get("tags") or []),
        )
        for i, s in enumerate(spec.get("scenarios") or [])
    ]

    async def subject(ctx: EvalContext, sc: Scenario) -> Any:
        inputs = {**(wf_spec.get("inputs") or {}), **sc.inputs}
        return await workflow.run(ctx.engine, inputs=inputs, provider=provider, project=project, announce=False)

    return subject, scenarios, str(spec.get("name", Path(path).stem))
