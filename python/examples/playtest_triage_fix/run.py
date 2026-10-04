"""Playtest -> triage -> fix -> verify on examples/sky_dash, with Python agents in the engine's studio.

    python run.py --dry-run                 # scripted models: no API key, runs in seconds
    python run.py                           # Claude (ANTHROPIC_API_KEY), on a copy of sky_dash
    python run.py --attach                  # on the project open in the editor: watch it live

What happens (and what you see in the editor's Studio panel when attached):
1. ``playtest``: the engine's bot plays the level (goal_seeker) and reports metrics.
2. ``triage``: Nimbus (creative director) reads the report, files feedback and decides with
   ``studio_decide`` (tasks on the board, assigned to Stratus).
3. ``fix``: Stratus claims its tasks, edits the scene, verifies, marks them done, and remembers the
   lesson in shared memory.
4. ``verify``: the bot plays again; ``playtest_compare`` records the measured effect on the feedback.
Repeats until every run reaches the goal (or 3 iterations).
"""

from __future__ import annotations

import argparse
import asyncio
import json
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Any

from skywalker_agents import AsyncEngine, Memory, RunContext, ScriptedProvider, Workflow
from skywalker_agents.harness.approvals import AutoApprove

HERE = Path(__file__).resolve().parent
SKY_DASH = HERE.parents[2] / "examples" / "sky_dash"
PLAYTEST = {"policy": "goal_seeker", "runs": 2, "seconds": 30, "include_heatmap": False, "screenshots": False}
TARGET = 1.0  # completion_rate


def build_workflow() -> Workflow:
    wf = Workflow("playtest_triage_fix")

    @wf.step()
    async def playtest(ctx: RunContext) -> dict[str, Any]:
        assert ctx.engine is not None
        report = await ctx.engine.call("playtest_run", {**PLAYTEST, "label": f"iteration {ctx.iteration}"}, check=True)
        return {
            "id": report["id"],
            "metrics": report["metrics"],
            "warnings": report.get("warnings", []),
            "findings": report.get("findings", []),
        }

    @wf.step(after=["playtest"], when=lambda ctx: ctx.result("playtest")["metrics"]["completion_rate"] < TARGET)
    async def triage(ctx: RunContext) -> Any:
        pt = ctx.result("playtest")
        return await ctx.agent("nimbus").run(
            f"Playtest {pt['id']} of Sky Dash: metrics {json.dumps(pt['metrics'])}; warnings {pt['warnings']}; "
            f"findings {json.dumps(pt['findings'])}. Target: every run reaches the goal.\n"
            "File one feedback item per real problem with studio_feedback_submit (evidence: the playtest id), then "
            "decide each with studio_decide: act with a small task for @stratus (clear acceptance criteria), or "
            "drop it with a rationale. Report what you decided."
        )

    @wf.step(after=["triage"], when=lambda ctx: ctx.result("triage") is not None)
    async def fix(ctx: RunContext) -> Any:
        return await ctx.agent("stratus").run(
            "Claim your open tasks (studio_task_claim), fix each in the scene, verify the fix (scene_query, "
            "viewport_capture or sim_trace), mark the task done with studio_task_update, and remember the lesson "
            "with memory_remember (scope project, kind fact). Report briefly."
        )

    @wf.step(after=["fix"], when=lambda ctx: ctx.result("fix") is not None)
    async def verify(ctx: RunContext) -> dict[str, Any]:
        assert ctx.engine is not None
        before = ctx.result("playtest")
        after = await ctx.engine.call("playtest_run", {**PLAYTEST, "label": f"verify {ctx.iteration}"}, check=True)
        # Record the measured effect on every fixed feedback item (improved -> verified, regressed -> reopened).
        fixed = await ctx.engine.call("studio_feedback_list", {"status": "fixed"})
        effects = {}
        for item in fixed.get("feedback", []):
            res = await ctx.engine.call(
                "playtest_compare", {"before": before["id"], "after": after["id"], "feedback": item["id"]}
            )
            effects[item["id"]] = res.text.splitlines()[0] if res.text else ""
        return {"id": after["id"], "metrics": after["metrics"], "effects": effects}

    def done(ctx: RunContext) -> bool:
        last = ctx.result("verify") or ctx.result("playtest")
        return bool(last and last["metrics"]["completion_rate"] >= TARGET)

    wf.repeat(until=done, max_iterations=3)
    return wf


def scripted() -> ScriptedProvider:
    """What a good run looks like, as scripted model turns (the tools really run in the engine)."""
    script = json.loads((HERE / "script.json").read_text())
    return ScriptedProvider(script.get("default", []), agents=script["agents"])


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dry-run", action="store_true", help="scripted models (no API key)")
    ap.add_argument("--attach", nargs="?", const="", default=None, help="use the running editor's project")
    ap.add_argument("--project", default=None, help="project to work on (default: a temp copy of sky_dash)")
    ap.add_argument("--binary", default=None)
    args = ap.parse_args()

    if args.attach is not None:
        engine = await AsyncEngine.attach(args.attach or None)
        project = engine.project or "."
    else:
        project = args.project or str(Path(tempfile.mkdtemp(prefix="sky_dash_")) / "sky_dash")
        if not args.project:
            shutil.copytree(SKY_DASH, project, ignore=shutil.ignore_patterns("studio", "* 2.*"))
        engine = await AsyncEngine.spawn(project, binary=args.binary)
    memory = Memory.open(project)
    try:
        roster = await engine.call("studio_agent_list", {})
        if not {"nimbus", "stratus"} <= {a["id"] for a in roster.get("agents", [])}:
            await engine.call("studio_team_template", {"template": "indie_trio"}, check=True)
        result = await build_workflow().run(
            engine,
            provider=scripted() if args.dry_run else None,
            memory=memory,
            approver=AutoApprove(),
            project=project,
        )
    finally:
        await engine.close()
        await memory.close()
    first = result.history[0]["playtest"]["metrics"] if result.history else result.results["playtest"]["metrics"]
    last = (result.results.get("verify") or result.results["playtest"])["metrics"]
    print(
        f"{result.status}: completion {first['completion_rate']} -> {last['completion_rate']} "
        f"in {result.iterations} iteration(s), ${result.cost['total_usd']:.4f}"
    )
    print(f"trace: sky-agents trace show {result.run_id} --project {project}")
    return 0 if result.ok and last["completion_rate"] >= TARGET else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
