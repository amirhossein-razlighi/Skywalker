"""Two agents with shared memory: a level designer builds, a critic reviews what it sees, until approved.

    python run.py --dry-run          # scripted models, no API key
    python run.py                    # Claude: Mira on claude-opus-5-5, the critic on claude-sonnet-5-5
    python run.py --attach           # in the editor: watch the platforms appear and the thread in #general

Mira (level designer) adds a jump section to Sky Dash. Vera (critic) looks at a capture of the scene,
judges it against the brief, and stores her standards in project memory (``memory_remember``). On the
next round Mira's prompt automatically includes those memories (``Agent(memory=...)`` recalls what is
relevant), so the critique sticks, even across runs: the memory lives in ``<project>/studio/memory.sqlite``.
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
from skywalker_agents.harness import director_critic

HERE = Path(__file__).resolve().parent
SKY_DASH = HERE.parents[2] / "examples" / "sky_dash"
BRIEF = (
    "Add a three-platform jump section between x=40 and x=56 on Sky Dash (z=0, platforms 3 m wide, rising "
    "toward the goal). It must be fun and fair for the runner. Name them 'Jump Platform A/B/C'."
)
ROSTER = [
    {"id": "mira", "name": "Mira", "role": "level_designer", "focus": "jump sections and pacing"},
    {"id": "vera", "name": "Vera", "role": "critic", "focus": "fairness and readability", "model": "claude-sonnet-5-5"},
]


def build_workflow() -> Workflow:
    wf = Workflow("level_design_critic")

    @wf.step()
    async def roster(ctx: RunContext) -> list[str]:
        assert ctx.engine is not None
        for spec in ROSTER:
            await ctx.engine.call("studio_agent_define", spec, check=True)
        return [s["id"] for s in ROSTER]

    @wf.step(after=["roster"])
    async def design(ctx: RunContext) -> dict[str, Any]:
        return await director_critic(
            ctx,
            maker="mira",
            critic="vera",
            task=BRIEF,
            rounds=3,
            capture={"width": 640, "height": 360, "samples": 1, "eye": [48, 6, 22], "target": [48, 2, 0]},
        )

    @wf.step(after=["design"])
    async def debrief(ctx: RunContext) -> str:
        assert ctx.memory is not None
        return await ctx.memory.summarize("jump gaps platforms", kinds=["fact"])

    return wf


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--attach", nargs="?", const="", default=None)
    ap.add_argument("--project", default=None)
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
    provider = None
    if args.dry_run:
        script = json.loads((HERE / "script.json").read_text())
        provider = ScriptedProvider(agents=script["agents"])
    memory = Memory.open(project)
    try:
        result = await build_workflow().run(engine, provider=provider, memory=memory, project=project)
    finally:
        await engine.close()
        await memory.close()
    rounds = result.results.get("design", {}).get("rounds", [])
    for r in rounds:
        print(f"round {r['round']}: critic says {r['critic'][:120]!r}")
    print(
        f"{result.status}; approved={result.results.get('design', {}).get('approved')}; "
        f"${result.cost['total_usd']:.4f}\nshared memory:\n{result.results.get('debrief', '')}"
    )
    return 0 if result.ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
