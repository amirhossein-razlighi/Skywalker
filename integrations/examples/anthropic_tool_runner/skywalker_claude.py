#!/usr/bin/env python3
"""Drive Skywalker from the Anthropic API with the SDK's tool runner and MCP helpers.

Setup:
    pip install "anthropic[mcp]"         # Python 3.10+; https://github.com/anthropics/anthropic-sdk-python/blob/main/helpers.md
    export ANTHROPIC_API_KEY=...
    export ANTHROPIC_MODEL=...           # optional (defaults to claude-sonnet-5-5)
    python3 skywalker_claude.py "Make a foggy pine forest at dawn and show me a capture" --project ./MyGame

The engine runs headless over MCP stdio (`skywalker mcp --project DIR`). The skywalker-core skill is used as the
system prompt, and the MCP prompt `look_dev` / resources such as `skywalker://docs/RENDERING` are available the same way
(`mcp_client.get_prompt`, `mcp_client.read_resource`).
"""
from __future__ import annotations

import argparse
import asyncio
import os
import shutil
import sys
from pathlib import Path

from anthropic import AsyncAnthropic
from anthropic.lib.tools.mcp import async_mcp_tool
from mcp import ClientSession
from mcp.client.stdio import StdioServerParameters, stdio_client

REPO = Path(__file__).resolve().parents[3]
CORE_SKILL = REPO / "integrations" / "claude-code" / "skills" / "skywalker-core" / "SKILL.md"


def system_prompt() -> str:
    if not CORE_SKILL.exists():
        return "You build games with the Skywalker engine through its MCP tools. Look (viewport_capture) before claiming a result."
    text = CORE_SKILL.read_text()
    return text.split("\n---\n", 1)[1].strip() if text.startswith("---") else text


async def run(task: str, project: Path, binary: str) -> None:
    client = AsyncAnthropic()
    params = StdioServerParameters(command=binary, args=["mcp", "--project", str(project)])
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as mcp_client:
            await mcp_client.initialize()
            tools = (await mcp_client.list_tools()).tools
            runner = await client.beta.messages.tool_runner(
                model=os.environ.get("ANTHROPIC_MODEL", "claude-sonnet-5-5"),
                max_tokens=4096,
                system=system_prompt(),
                messages=[{"role": "user", "content": task}],
                tools=[async_mcp_tool(t, mcp_client) for t in tools],
            )
            async for message in runner:
                for block in message.content:
                    if block.type == "text":
                        print(block.text)
                    elif block.type == "tool_use":
                        print(f"-> {block.name}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("task", help="what to build or change")
    ap.add_argument("--project", type=Path, default=Path("."))
    ap.add_argument("--binary", default=shutil.which("skywalker") or "skywalker")
    args = ap.parse_args()
    args.project.mkdir(parents=True, exist_ok=True)
    asyncio.run(run(args.task, args.project, args.binary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
