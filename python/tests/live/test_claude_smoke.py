"""One real Claude call through the agent loop. Off by default: needs SKY_AGENTS_LIVE=1 and ANTHROPIC_API_KEY.

The key is never printed or stored; the budget caps the spend.
"""

from __future__ import annotations

import os

import pytest

from skywalker_agents import Agent, AsyncEngine, Budget, Role

pytestmark = [
    pytest.mark.live,
    pytest.mark.skipif(
        os.environ.get("SKY_AGENTS_LIVE") != "1" or not os.environ.get("ANTHROPIC_API_KEY"),
        reason="set SKY_AGENTS_LIVE=1 and ANTHROPIC_API_KEY to run",
    ),
]


async def test_claude_uses_an_engine_tool() -> None:
    from skywalker_agents.llm.anthropic import AnthropicProvider

    engine = AsyncEngine.fake()
    await engine.call("entity_create", {"name": "Red Pillar"})
    agent = Agent(
        Role.adhoc("smoke", instructions="Answer in one short sentence."),
        engine=engine,
        provider=AnthropicProvider(),
        model="claude-sonnet-5-5",
        effort="low",
        max_tokens=4000,
        engine_tools=["scene_overview"],
        budget=Budget(max_cost_usd=0.10, max_turns=4),
    )
    res = await agent.run("Use scene_overview, then tell me the name of the only entity.")
    assert res.ok, res.error
    assert "Red Pillar" in res.text
    assert [c.name for c in res.tool_calls] == ["scene_overview"]
    assert res.usage.output_tokens > 0
