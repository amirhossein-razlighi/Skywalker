"""Skywalker tools inside a LangGraph agent (``pip install skywalker-agents[langgraph]``).

    python langgraph_adapter.py --offline              # a tiny scripted chat model, no API key
    python langgraph_adapter.py --model anthropic:claude-opus-5-5   # any LangChain chat model id

The engine tools are attributed to ``mcp:sky-agents/langgraph`` in the editor's Activity feed, and the
graph can use shared memory through the same ``memory_*`` tools Python agents use.
"""

from __future__ import annotations

import argparse
import asyncio
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Any

from langchain_core.language_models import BaseChatModel
from langchain_core.messages import AIMessage, BaseMessage
from langchain_core.outputs import ChatGeneration, ChatResult

try:  # LangChain 1.x: agents are LangGraph graphs built by create_agent
    from langchain.agents import create_agent as make_agent
except ImportError:  # langgraph alone
    import warnings

    from langgraph.prebuilt import create_react_agent as make_agent

    warnings.filterwarnings("ignore", message="create_react_agent has been moved")

from skywalker_agents import AsyncEngine, Memory
from skywalker_agents.adapters import engine_tools
from skywalker_agents.adapters.langchain import as_langchain_tools

SKY_DASH = Path(__file__).resolve().parents[2] / "examples" / "sky_dash"


class ScriptedChatModel(BaseChatModel):
    """Offline stand-in: asks for the hazards, remembers a fact, then answers."""

    step: int = 0

    @property
    def _llm_type(self) -> str:
        return "scripted"

    def bind_tools(self, tools: Any, **kwargs: Any) -> ScriptedChatModel:
        return self

    def _generate(
        self, messages: list[BaseMessage], stop: Any = None, run_manager: Any = None, **kwargs: Any
    ) -> ChatResult:
        self.step += 1
        if self.step == 1:
            msg = AIMessage(content="", tool_calls=[{"name": "scene_query", "args": {"tag": "hazard"}, "id": "c1"}])
        elif self.step == 2:
            msg = AIMessage(
                content="",
                tool_calls=[
                    {
                        "name": "memory_remember",
                        "id": "c2",
                        "args": {
                            "text": "Sky Dash has spikes every 14 m along the run",
                            "kind": "fact",
                            "scope": "project",
                        },
                    }
                ],
            )
        else:
            msg = AIMessage(content=f"Found the hazards; last tool said: {str(messages[-1].content)[:80]}")
        return ChatResult(generations=[ChatGeneration(message=msg)])


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--offline", action="store_true")
    ap.add_argument("--model", default="anthropic:claude-opus-5-5")
    ap.add_argument("--binary", default=None)
    args = ap.parse_args()
    project = Path(tempfile.mkdtemp(prefix="sky_dash_")) / "sky_dash"
    shutil.copytree(SKY_DASH, project, ignore=shutil.ignore_patterns("studio", "* 2.*"))
    engine = await AsyncEngine.spawn(project, binary=args.binary)
    memory = Memory.open(project, agent="langgraph")
    try:
        sky_tools = await engine_tools(engine, ["scene_query", "scene_overview", "entity_update"], agent="langgraph")
        tools = as_langchain_tools([*sky_tools, *memory.tools()])
        if args.offline:
            model: Any = ScriptedChatModel()
        else:
            from langchain.chat_models import init_chat_model

            model = init_chat_model(args.model)
        graph = make_agent(model, tools)
        out = await graph.ainvoke({"messages": [("user", "Which hazards does Sky Dash have? Remember the pattern.")]})
        print(out["messages"][-1].content)
        print("memory:", [h.item.text for h in await memory.recall("spikes")])
    finally:
        await engine.close()
        await memory.close()
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
