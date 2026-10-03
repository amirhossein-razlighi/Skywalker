#!/usr/bin/env python3
"""Drive Skywalker from the OpenAI Agents SDK over MCP (stdio).

Two things in one script:

  scene   a single agent builds a small scene (terrain, light, camera) and looks at it;
  studio  a studio loop: roster members run as separate Agents that share the engine through
          the same MCP server, while this script plays the loop driver
          (studio_loop_start -> run each assignment as its agent -> studio_loop_advance).

Setup:
    pip install openai-agents            # https://openai.github.io/openai-agents-python/
    export OPENAI_API_KEY=...            # never written to disk by Skywalker
    export OPENAI_MODEL=...              # optional; otherwise the SDK default is used
    python3 skywalker_agents.py scene  --project ./MyGame
    python3 skywalker_agents.py studio --project ./MyGame --loop playtest_fix_verify

The engine runs headless (`skywalker mcp --project DIR`). To co-edit with the editor instead, use
`skywalker mcp --auto` (attaches to a running editor, falls back to headless).
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import shutil
import sys
from pathlib import Path

from agents import Agent, Runner
from agents.mcp import MCPServerStdio

REPO = Path(__file__).resolve().parents[3]
CORE_SKILL = REPO / "integrations" / "claude-code" / "skills" / "skywalker-core" / "SKILL.md"
MODEL = os.environ.get("OPENAI_MODEL") or None


def engine_server(project: Path, binary: str) -> MCPServerStdio:
    return MCPServerStdio(
        name="skywalker",
        params={"command": binary, "args": ["mcp", "--project", str(project)]},
        cache_tools_list=True,               # ~110 tools: list them once
        client_session_timeout_seconds=300,  # captures, playtests and DCC jobs can take a while
    )


def core_instructions() -> str:
    """The skywalker-core skill as the agent's standing instructions (frontmatter stripped)."""
    if not CORE_SKILL.exists():
        return "You build games with the Skywalker engine through its MCP tools. Look (viewport_capture) before claiming a result."
    text = CORE_SKILL.read_text()
    return text.split("\n---\n", 1)[1].strip() if text.startswith("---") else text


def payload(result) -> dict:
    """The structured JSON of an MCP tool result (falls back to parsing the text block)."""
    if getattr(result, "structuredContent", None):
        return result.structuredContent
    for block in result.content:
        text = getattr(block, "text", "")
        if "{" in text:
            try:
                return json.loads(text[text.index("{"):])
            except json.JSONDecodeError:
                pass
    return {}


async def build_scene(project: Path, binary: str) -> None:
    async with engine_server(project, binary) as server:
        agent = Agent(
            name="Builder",
            model=MODEL,
            instructions=core_instructions(),
            mcp_servers=[server],
        )
        result = await Runner.run(
            agent,
            "Build a small island: terrain_create with the island_beach preset (size 200, water on), a few palm-like "
            "cones scattered on land with scatter (surface set to the terrain), golden-hour lighting, and a game camera at "
            "eye height above the ground (query the terrain height first). Capture a 16-sample beauty shot with overlays "
            "off, describe what you see honestly, then scene_save scenes/island.sky.json.",
            max_turns=40,
        )
        print(result.final_output)


async def run_studio(project: Path, binary: str, loop: str, iterations: int) -> None:
    async with engine_server(project, binary) as server:
        # 1. Make sure the roster exists (ids match the roles; see skywalker-studio).
        await server.call_tool("studio_team_template", {"template": "indie_trio"})
        overview = payload(await server.call_tool("studio_overview", {}))
        print("roster:", [a["id"] for a in overview.get("roster", [])])

        # 2. Define and start the loop; the engine runs playtest stages itself.
        await server.call_tool("studio_loop_define", {"template": loop, "goal": "Players can finish the level without unfair deaths."})
        status = payload(await server.call_tool("studio_loop_start", {"loop": loop, "max_iterations": iterations}))

        # 3. Execute assignments as the matching roster member until the loop ends.
        while status.get("status") == "running" and status.get("assignments"):
            reports = []
            for assignment in status["assignments"]:
                agent_id = assignment["agent"]
                brief = payload(await server.call_tool("studio_agent_brief", {"agent": agent_id, "loop_member": True}))
                member = Agent(
                    name=agent_id,
                    model=MODEL,
                    instructions=brief["system_prompt"] + f'\n\nPass as:"{agent_id}" on every studio_* call.\n\n' + core_instructions(),
                    mcp_servers=[server],
                )
                run = await Runner.run(member, assignment["prompt"], max_turns=40)
                reports.append({"agent": agent_id, "report": run.final_output})
                print(f"[{agent_id}] {str(run.final_output)[:200]}")
            status = payload(await server.call_tool("studio_loop_advance", {"loop": loop, "reports": reports}))
            if status.get("status") == "awaiting_approval":
                print("The loop waits for a human approval gate; approve it in the editor or call studio_loop_advance {approve:true}.")
                break
        print("loop status:", status.get("status"))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["scene", "studio"])
    ap.add_argument("--project", type=Path, default=Path("."), help="project folder (created if missing)")
    ap.add_argument("--binary", default=shutil.which("skywalker") or "skywalker", help="path to the skywalker executable")
    ap.add_argument("--loop", default="playtest_fix_verify", help="studio loop name or template")
    ap.add_argument("--iterations", type=int, default=2)
    args = ap.parse_args()
    args.project.mkdir(parents=True, exist_ok=True)
    if args.mode == "scene":
        asyncio.run(build_scene(args.project, args.binary))
    else:
        asyncio.run(run_studio(args.project, args.binary, args.loop, args.iterations))
    return 0


if __name__ == "__main__":
    sys.exit(main())
