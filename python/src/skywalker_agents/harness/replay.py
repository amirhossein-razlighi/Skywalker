"""Deterministic record/replay of model and tool calls (cassettes), for evals and regression tests.

    rec = Recorder("runs/golden.cassette.jsonl")          # record a live run
    await wf.run(engine, middleware=[rec])

    rep = Replayer("runs/golden.cassette.jsonl", tools=True)   # replay it: no model calls, no engine edits
    await wf.run(engine, middleware=[rep], provider=rep.provider())

Keys hash what a call depends on (model, system, conversation, tool names / tool name and
arguments), so a replay follows the recording exactly and a strict replay fails loudly
(:class:`ReplayMismatch`) on the first divergence.
"""

from __future__ import annotations

import hashlib
import json
import os
import threading
from collections import defaultdict, deque
from pathlib import Path
from typing import Any, Literal

from ..engine.results import ToolResult
from ..errors import ReplayMismatch
from ..hooks import LLMNext, Middleware, ToolInvocation, ToolNext
from ..llm.types import LLMRequest, LLMResponse


def llm_key(req: LLMRequest) -> str:
    msgs = [{"role": m.role, "content": [b.model_dump(mode="json") for b in m.content]} for m in req.messages]
    # The model id is left out on purpose: a replay runs with a stand-in provider.
    payload = {
        "agent": req.metadata.get("agent", ""),
        "system": req.system,
        "messages": msgs,
        "tools": sorted(t.name for t in req.tools),
    }
    return hashlib.sha256(json.dumps(payload, sort_keys=True, default=str).encode()).hexdigest()[:24]


def tool_key(agent: str, name: str, args: dict[str, Any]) -> str:
    payload = {"agent": agent, "tool": name, "args": args}
    return hashlib.sha256(json.dumps(payload, sort_keys=True, default=str).encode()).hexdigest()[:24]


class Recorder(Middleware):
    """Appends every model response and tool result to a JSONL cassette."""

    def __init__(self, path: str | os.PathLike[str], *, tools: bool = True) -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.tools = tools
        self._lock = threading.Lock()
        self.count = 0

    def _write(self, entry: dict[str, Any]) -> None:
        with self._lock, self.path.open("a", encoding="utf-8") as f:
            f.write(json.dumps(entry, default=str, ensure_ascii=False) + "\n")
            self.count += 1

    async def on_llm(self, request: LLMRequest, call_next: LLMNext) -> LLMResponse:
        resp = await call_next(request)
        self._write(
            {
                "type": "llm",
                "key": llm_key(request),
                "agent": request.metadata.get("agent", ""),
                "response": resp.model_dump(mode="json"),
            }
        )
        return resp

    async def on_tool(self, inv: ToolInvocation, call_next: ToolNext) -> ToolResult:
        result = await call_next(inv)
        if self.tools:
            self._write(
                {
                    "type": "tool",
                    "key": tool_key(inv.agent_id, inv.tool.name, inv.args),
                    "agent": inv.agent_id,
                    "tool": inv.tool.name,
                    "args": inv.args,
                    "result": result.to_mcp(),
                }
            )
        return result


class Replayer(Middleware):
    """Serves recorded responses (and tool results when ``tools=True``) instead of calling out.

    ``mode="strict"`` raises on anything unrecorded; ``"fallthrough"`` calls the real thing instead.
    """

    def __init__(
        self, path: str | os.PathLike[str], *, tools: bool = True, mode: Literal["strict", "fallthrough"] = "strict"
    ) -> None:
        self.tools = tools
        self.mode = mode
        self._llm: dict[str, deque[dict[str, Any]]] = defaultdict(deque)
        self._tool: dict[str, deque[dict[str, Any]]] = defaultdict(deque)
        self.served = 0
        self.misses: list[str] = []
        for line in Path(path).read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            e = json.loads(line)
            (self._llm if e["type"] == "llm" else self._tool)[e["key"]].append(e)

    def provider(self) -> ReplayProvider:
        """A provider stand-in so no model client (or API key) is needed during replay."""
        return ReplayProvider(self)

    async def on_llm(self, request: LLMRequest, call_next: LLMNext) -> LLMResponse:
        key = llm_key(request)
        q = self._llm.get(key)
        if q:
            self.served += 1
            return LLMResponse.model_validate(q.popleft()["response"])
        self.misses.append(f"llm:{request.metadata.get('agent', '')}:{key}")
        if self.mode == "strict":
            raise ReplayMismatch(
                f"no recorded model response for agent '{request.metadata.get('agent', '')}' "
                f"turn {request.metadata.get('turn', '?')} (key {key})"
            )
        return await call_next(request)

    async def on_tool(self, inv: ToolInvocation, call_next: ToolNext) -> ToolResult:
        if not self.tools:
            return await call_next(inv)
        key = tool_key(inv.agent_id, inv.tool.name, inv.args)
        q = self._tool.get(key)
        if q:
            self.served += 1
            return ToolResult.from_mcp(inv.tool.name, q.popleft()["result"])
        self.misses.append(f"tool:{inv.tool.name}:{key}")
        if self.mode == "strict":
            raise ReplayMismatch(f"no recorded result for {inv.tool.name} {json.dumps(inv.args, default=str)[:200]}")
        return await call_next(inv)


class ReplayProvider:
    """Answers only from a :class:`Replayer` (which, as middleware, intercepts first)."""

    name = "replay"
    default_model = "replay"
    supports_vision = True

    def __init__(self, replayer: Replayer) -> None:
        self.replayer = replayer

    async def complete(self, request: LLMRequest) -> LLMResponse:
        raise ReplayMismatch(f"unrecorded model call (key {llm_key(request)}); add the Replayer as middleware")
