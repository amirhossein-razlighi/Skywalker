"""Reusable multi-agent patterns. Each takes a :class:`RunContext` (agents, engine, memory, board,
messaging, budgets and tracing come from the run) and returns plain data, so they compose inside
workflows and are callable from declarative specs (``kind: pattern``).

* :func:`director_critic` - a maker works, a critic reviews (with a capture), until approved.
* :func:`debate` - agents argue in rounds; a judge decides.
* :func:`plan_and_execute` - a planner writes a plan onto the board; executors claim and do it.
* :func:`map_reduce` - fan an agent (or function) out over items, then reduce.
* :func:`playtest_triage_fix_verify` - the studio's core loop, driven from Python.
"""

from __future__ import annotations

import functools
import json
import re
from collections.abc import Awaitable, Callable, Sequence
from typing import TYPE_CHECKING, Any

from .retry import gather_limited

if TYPE_CHECKING:  # pragma: no cover
    from .workflow import RunContext


def _extract_json(text: str) -> Any:
    """The first JSON object or array in a model's text (fenced or bare)."""
    m = re.search(r"```(?:json)?\s*(.+?)```", text, re.S)
    candidates = [m.group(1)] if m else []
    candidates += re.findall(r"(\[.*\]|\{.*\})", text, re.S)
    for c in candidates:
        try:
            return json.loads(c)
        except json.JSONDecodeError:
            continue
    return None


async def director_critic(
    ctx: RunContext,
    *,
    maker: str,
    critic: str,
    task: str,
    rounds: int = 3,
    approve_word: str = "APPROVED",
    capture: bool | dict[str, Any] = True,
) -> dict[str, Any]:
    """The maker works; the critic reviews (seeing a viewport capture; ``capture`` may hold viewport_capture
    arguments) and either approves or lists fixes."""
    history: list[dict[str, Any]] = []
    feedback = ""
    for r in range(1, rounds + 1):
        prompt = task if not feedback else f"{task}\n\nThe critic's feedback on your last attempt:\n{feedback}"
        made = await ctx.agent(maker).run(prompt)
        images: list[bytes] = []
        if capture and ctx.engine is not None:
            args = {"width": 768, "height": 432, "annotate": False, **(capture if isinstance(capture, dict) else {})}
            shot = await ctx.engine.call("viewport_capture", args)
            images = shot.images[:1]
        review = await ctx.agent(critic).run(
            f"Review this work against the brief. Brief: {task}\n\nThe maker's report:\n{made.text}\n\n"
            f"If it fully meets the brief, start your answer with {approve_word}. Otherwise list the concrete fixes, "
            f"most important first.",
            images=images,
        )
        approved = review.text.strip().upper().startswith(approve_word.upper())
        history.append({"round": r, "maker": made.text, "critic": review.text, "approved": approved})
        if approved:
            break
        feedback = review.text
    return {"approved": bool(history and history[-1]["approved"]), "rounds": history}


async def debate(
    ctx: RunContext, *, agents: Sequence[str], question: str, rounds: int = 2, judge: str | None = None
) -> dict[str, Any]:
    """Each agent answers, then rebuts the others for ``rounds``; the judge (if any) gives the verdict."""
    transcript: list[dict[str, str]] = []
    for r in range(1, rounds + 1):
        so_far = "\n\n".join(f"@{t['agent']} (round {t['round']}): {t['text']}" for t in transcript)
        prompt = f"Question: {question}\n\n" + (
            f"The debate so far:\n{so_far}\n\nRebut or refine, briefly."
            if so_far
            else "Give your position and your best argument, briefly."
        )
        turns = await gather_limited([functools.partial(ctx.agent(a).run, prompt) for a in agents], len(agents))
        for a, res in zip(agents, turns, strict=True):
            transcript.append({"agent": a, "round": str(r), "text": getattr(res, "text", str(res))})
    verdict = ""
    if judge:
        everything = "\n\n".join(f"@{t['agent']} (round {t['round']}): {t['text']}" for t in transcript)
        verdict = (
            await ctx.agent(judge).run(
                f"Question: {question}\n\nDebate:\n{everything}\n\nDecide. Give the decision first, then why."
            )
        ).text
    return {"question": question, "transcript": transcript, "verdict": verdict}


async def plan_and_execute(
    ctx: RunContext, *, planner: str, executors: Sequence[str], goal: str, max_steps: int = 6, review: bool = True
) -> dict[str, Any]:
    """The planner writes a JSON plan; each step becomes a board task for an executor; the planner reviews."""
    plan_res = await ctx.agent(planner).run(
        f"Goal: {goal}\n\nWrite a plan of at most {max_steps} small, checkable steps as a JSON array: "
        f'[{{"title": ..., "assignee": one of {list(executors)}, "instructions": ..., "acceptance": [...]}}]. '
        "Only the JSON."
    )
    plan = _extract_json(plan_res.text)
    if not isinstance(plan, list):
        plan = [{"title": goal, "assignee": executors[0], "instructions": goal, "acceptance": []}]
    plan = plan[:max_steps]
    tasks: list[tuple[str, dict[str, Any], dict[str, Any]]] = []
    for st in plan:
        who = st.get("assignee") if st.get("assignee") in executors else executors[len(tasks) % len(executors)]
        task = await ctx.board.post(
            str(st.get("title", "step")),
            description=str(st.get("instructions", "")),
            acceptance=[str(x) for x in st.get("acceptance", [])],
            assignee=who,
            by=planner,
        )
        tasks.append((who, task, st))

    async def execute(who: str, task: dict[str, Any], st: dict[str, Any]) -> dict[str, Any]:
        await ctx.board.update(task["id"], by=who, status="doing")
        res = await ctx.agent(who).run(
            f"Board task {task['id']}: {st.get('title')}\n{st.get('instructions', '')}\n"
            f"Acceptance: {st.get('acceptance', [])}"
        )
        await ctx.board.complete(task["id"], by=who, report=res.text[:1000])
        return {"task": task["id"], "agent": who, "report": res.text}

    done = await gather_limited([functools.partial(execute, *t) for t in tasks], limit=len(executors))
    reports = [d for d in done if isinstance(d, dict)]
    verdict = ""
    if review:
        verdict = (
            await ctx.agent(planner).run(
                f"Goal: {goal}\nReports:\n"
                + "\n".join(f"- {r['task']} @{r['agent']}: {r['report'][:500]}" for r in reports)
                + "\nIs the goal met? Answer DONE or list what is missing."
            )
        ).text
    return {"plan": plan, "reports": reports, "review": verdict}


Mapper = str | Callable[["RunContext", Any], Awaitable[Any]]


async def map_reduce(
    ctx: RunContext,
    *,
    items: Sequence[Any],
    mapper: Mapper,
    reducer: Mapper | None = None,
    prompt: str = "{item}",
    concurrency: int = 4,
) -> dict[str, Any]:
    """Runs ``mapper`` (an agent id with a ``prompt`` template, or a function) over ``items``, then ``reducer``."""

    async def one(item: Any) -> Any:
        if isinstance(mapper, str):
            text = prompt.replace("{item}", json.dumps(item, default=str) if not isinstance(item, str) else item)
            return (await ctx.agent(mapper).run(text)).text
        return await mapper(ctx, item)

    mapped = await gather_limited([functools.partial(one, it) for it in items], concurrency)
    results = [m if not isinstance(m, BaseException) else f"error: {m}" for m in mapped]
    reduced: Any = None
    if reducer is not None:
        if isinstance(reducer, str):
            listing = "\n".join(f"- {json.dumps(i, default=str)}: {r}" for i, r in zip(items, results, strict=True))
            reduced = (await ctx.agent(reducer).run(f"Combine these results into one answer:\n{listing}")).text
        else:
            reduced = await reducer(ctx, results)
    return {"results": results, "reduced": reduced}


async def playtest_triage_fix_verify(
    ctx: RunContext,
    *,
    director: str,
    fixers: Sequence[str],
    playtest: dict[str, Any] | None = None,
    targets: dict[str, Any] | None = None,
    iterations: int = 3,
) -> dict[str, Any]:
    """Playtest -> file findings -> the director triages (``studio_decide``) -> fixers do their board tasks ->
    verification playtest with before/after comparison, until ``targets`` are met."""
    if ctx.engine is None:
        raise RuntimeError("playtest_triage_fix_verify needs an engine")
    engine = ctx.engine
    args = {"policy": "goal_seeker", "runs": 2, "seconds": 30, "include_heatmap": False, **(playtest or {})}
    history = []
    report = (await engine.call("playtest_run", args, check=True)).data
    for it in range(1, iterations + 1):
        metrics = report.get("metrics", {})
        if targets and _met(metrics, targets):
            history.append({"iteration": it, "metrics": metrics, "met": True})
            break
        for f in report.get("findings", []):
            await engine.call(
                "studio_feedback_submit",
                {
                    **{k: f[k] for k in ("category", "severity", "summary", "details", "fingerprint") if k in f},
                    "evidence": {"playtest": report.get("id")},
                },
            )
        open_fb = (await engine.call("studio_feedback_list", {"status": "needs_decision"})).data.get("feedback", [])
        if open_fb:
            listing = "\n".join(f"- {f['id']} [{f.get('severity')}] {f['summary']}" for f in open_fb)
            await ctx.agent(director).run(
                f"Playtest {report.get('id')} metrics: {json.dumps(metrics)}\nTargets: {json.dumps(targets or {})}\n"
                f"Open feedback:\n{listing}\n\nTriage each item with studio_decide (act with tasks assigned to "
                f"{', '.join('@' + f for f in fixers)}, or drop/defer with a rationale)."
            )

        async def fix(who: str) -> str:
            tasks = await ctx.board.tasks("open", assignee=who)
            if not tasks:
                return ""
            listing = "\n".join(f"- {t['id']}: {t['title']} (accept: {t.get('acceptance', [])})" for t in tasks)
            res = await ctx.agent(who).run(
                f"Your tasks:\n{listing}\nDo them, verify each, and mark them done with studio_task_update."
            )
            return res.text

        await gather_limited([functools.partial(fix, w) for w in fixers], len(fixers))
        before = report
        report = (await engine.call("playtest_run", {**args, "label": f"verify {it}"}, check=True)).data
        compare = await engine.call("playtest_compare", {"before": before.get("id"), "after": report.get("id")})
        history.append(
            {
                "iteration": it,
                "before": before.get("metrics"),
                "after": report.get("metrics"),
                "compare": compare.data if not compare.is_error else compare.text,
            }
        )
    final = report.get("metrics", {})
    return {
        "metrics": final,
        "met": bool(targets) and _met(final, targets or {}),
        "iterations": history,
        "playtest": report.get("id"),
    }


def _met(metrics: dict[str, Any], targets: dict[str, Any]) -> bool:
    for k, rule in targets.items():
        v = metrics.get(k)
        if v is None:
            return False
        if isinstance(rule, dict):
            if "min" in rule and v < rule["min"]:
                return False
            if "max" in rule and v > rule["max"]:
                return False
        elif v != rule:
            return False
    return True


PATTERNS: dict[str, Callable[..., Awaitable[dict[str, Any]]]] = {
    "director_critic": director_critic,
    "debate": debate,
    "plan_and_execute": plan_and_execute,
    "map_reduce": map_reduce,
    "playtest_triage_fix_verify": playtest_triage_fix_verify,
}
