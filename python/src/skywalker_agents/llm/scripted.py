"""Deterministic model stand-ins: scripted conversations and callables (tests, dry runs, evals).

The script format matches ``skywalker studio run --mock``::

    {"agents": {"stratus": [{"text": "...", "tool_calls": [{"name": "entity_create", "input": {...}}]},
                            {"text": "Done."}]},
     "default": [{"text": "Nothing to do."}]}
"""

from __future__ import annotations

import itertools
import json
from collections.abc import Callable
from pathlib import Path
from typing import Any

from .types import LLMRequest, LLMResponse, StopReason, TextBlock, ToolCall, Usage

Turn = dict[str, Any] | Callable[[LLMRequest], "LLMResponse | dict[str, Any]"]


class ScriptedProvider:
    """Plays back scripted turns, per agent (``request.metadata['agent']``) or shared."""

    name = "scripted"
    default_model = "scripted"
    supports_vision = True

    def __init__(
        self,
        turns: list[Turn] | None = None,
        *,
        agents: dict[str, list[Turn]] | None = None,
        exhausted: str = "(script exhausted)",
        usage: Usage | None = None,
    ) -> None:
        self.default = list(turns or [])
        self.agents = {k: list(v) for k, v in (agents or {}).items()}
        self.exhausted = exhausted
        self.usage = usage or Usage(input_tokens=100, output_tokens=20, requests=1)
        self.requests: list[LLMRequest] = []
        self._ids = itertools.count(1)

    @classmethod
    def from_file(cls, path: str | Path) -> ScriptedProvider:
        data = json.loads(Path(path).read_text())
        return cls(data.get("default", []), agents=data.get("agents", {}))

    async def complete(self, request: LLMRequest) -> LLMResponse:
        self.requests.append(request)
        agent = str(request.metadata.get("agent", ""))
        queue = self.agents.get(agent) if agent in self.agents else self.default
        if not queue:
            return LLMResponse(
                content=[TextBlock(text=self.exhausted)],
                stop_reason="end_turn",
                usage=self.usage,
                model=self.default_model,
                provider=self.name,
            )
        turn = queue.pop(0)
        out = turn(request) if callable(turn) else turn
        if isinstance(out, LLMResponse):
            return out
        return self.turn_to_response(out, self.default_model)

    def turn_to_response(self, turn: dict[str, Any], model: str) -> LLMResponse:
        blocks: list[Any] = []
        if turn.get("text"):
            blocks.append(TextBlock(text=str(turn["text"])))
        for call in turn.get("tool_calls", []):
            blocks.append(
                ToolCall(
                    id=str(call.get("id") or f"call_{next(self._ids)}"),
                    name=call["name"],
                    input=dict(call.get("input") or {}),
                )
            )
        stop: StopReason = turn.get("stop") or ("tool_use" if turn.get("tool_calls") else "end_turn")
        usage = Usage.model_validate(turn["usage"]) if "usage" in turn else self.usage
        return LLMResponse(content=blocks, stop_reason=stop, usage=usage, model=model, provider=self.name)


class EchoProvider:
    """Answers every request with its last user text (smoke tests, plumbing checks)."""

    name = "echo"
    default_model = "echo"
    supports_vision = True

    async def complete(self, request: LLMRequest) -> LLMResponse:
        last = next((m.text for m in reversed(request.messages) if m.role == "user" and m.text), "")
        return LLMResponse(
            content=[TextBlock(text=last)],
            stop_reason="end_turn",
            usage=Usage(input_tokens=len(last) // 4, output_tokens=len(last) // 4, requests=1),
            model="echo",
            provider="echo",
        )
