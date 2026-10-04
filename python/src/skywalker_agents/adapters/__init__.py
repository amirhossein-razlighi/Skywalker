"""Thin adapters to other agent frameworks. Each module imports its framework lazily (optional extras).

| Framework | Adapter |
|---|---|
| LangChain / LangGraph | :func:`langchain.as_langchain_tools` |
| OpenAI Agents SDK | :func:`openai_agents.as_openai_agents_tools`, :func:`openai_agents.skywalker_mcp_params` |
| Pydantic AI | :func:`pydantic_ai.as_pydantic_ai_tools` |
| CrewAI | :func:`crewai.as_crewai_tools` |
| Claude Agent SDK | :func:`claude_agent_sdk.as_sdk_mcp_server`, :func:`claude_agent_sdk.skywalker_mcp_config` |
| Any MCP client | :mod:`mcp_server` (``sky-agents serve-mcp``) |

All of them take :class:`~skywalker_agents.tools.Tool` objects: engine tools (``engine_tools(...)``),
memory tools, or your own ``@tool`` functions.
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import TYPE_CHECKING

from ..tools.base import Tool, engine_tool

if TYPE_CHECKING:  # pragma: no cover
    from ..engine.client import AgentSession, AsyncEngine


async def engine_tools(
    engine: AsyncEngine, names: Sequence[str] | None = None, *, agent: str | None = None
) -> list[Tool]:
    """Engine tools as :class:`Tool` objects (attributed to ``agent`` when given)."""
    caller: AgentSession | AsyncEngine = engine.as_agent(agent) if agent else engine
    specs = await engine.list_tools()
    wanted = set(names) if names is not None else None
    return [
        engine_tool(caller, s.model_dump(by_alias=True))
        for s in specs
        if (wanted is None or s.name in wanted) and s.category != "agent"
    ]


__all__ = ["engine_tools"]
