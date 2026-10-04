"""Middleware around tool calls and LLM calls: logging, redaction, guardrails, caching, recording.

A middleware overrides either or both hooks and calls ``call_next`` to continue the chain (or
returns its own result to short-circuit)::

    class Audit(Middleware):
        async def on_tool(self, inv, call_next):
            print(inv.agent_id, inv.tool.name, inv.args)
            return await call_next(inv)

    agent = Agent(..., middleware=[Audit(), RedactionMiddleware()])

The first middleware in the list is the outermost.
"""

from __future__ import annotations

import fnmatch
import json
import logging
import re
import time
from collections.abc import Awaitable, Callable, Sequence
from dataclasses import dataclass, field
from typing import Any

from .engine.results import ToolResult
from .llm.types import ChatMessage, LLMRequest, LLMResponse, TextBlock, ToolResultBlock
from .tools.base import Tool, ToolContext


@dataclass
class ToolInvocation:
    tool: Tool
    args: dict[str, Any]
    ctx: ToolContext
    call_id: str = ""
    metadata: dict[str, Any] = field(default_factory=dict)

    @property
    def agent_id(self) -> str:
        return self.ctx.agent_id


ToolNext = Callable[[ToolInvocation], Awaitable[ToolResult]]
LLMNext = Callable[[LLMRequest], Awaitable[LLMResponse]]


class Middleware:
    """Pass-through base class."""

    async def on_tool(self, inv: ToolInvocation, call_next: ToolNext) -> ToolResult:
        return await call_next(inv)

    async def on_llm(self, request: LLMRequest, call_next: LLMNext) -> LLMResponse:
        return await call_next(request)


def chain_tool(middleware: Sequence[Middleware], final: ToolNext) -> ToolNext:
    handler = final
    for mw in reversed(middleware):
        handler = _bind_tool(mw, handler)
    return handler


def chain_llm(middleware: Sequence[Middleware], final: LLMNext) -> LLMNext:
    handler = final
    for mw in reversed(middleware):
        handler = _bind_llm(mw, handler)
    return handler


def _bind_tool(mw: Middleware, nxt: ToolNext) -> ToolNext:
    async def run(inv: ToolInvocation) -> ToolResult:
        return await mw.on_tool(inv, nxt)

    return run


def _bind_llm(mw: Middleware, nxt: LLMNext) -> LLMNext:
    async def run(req: LLMRequest) -> LLMResponse:
        return await mw.on_llm(req, nxt)

    return run


# ---------------------------------------------------------------------- built-ins
class LoggingMiddleware(Middleware):
    """Logs every tool call and model call (to the ``skywalker_agents`` logger)."""

    def __init__(self, logger: logging.Logger | None = None) -> None:
        self.log = logger or logging.getLogger("skywalker_agents")

    async def on_tool(self, inv: ToolInvocation, call_next: ToolNext) -> ToolResult:
        t0 = time.monotonic()
        result = await call_next(inv)
        self.log.info(
            "tool %s by %s -> %s (%.0f ms)",
            inv.tool.name,
            inv.agent_id or "?",
            "error" if result.is_error else "ok",
            (time.monotonic() - t0) * 1000,
        )
        return result

    async def on_llm(self, request: LLMRequest, call_next: LLMNext) -> LLMResponse:
        t0 = time.monotonic()
        resp = await call_next(request)
        self.log.info(
            "llm %s for %s -> %s, %d in / %d out (%.1f s)",
            resp.model,
            request.metadata.get("agent", "?"),
            resp.stop_reason,
            resp.usage.input_tokens,
            resp.usage.output_tokens,
            time.monotonic() - t0,
        )
        return resp


DEFAULT_SECRET_PATTERNS = [
    r"sk-ant-[A-Za-z0-9_\-]{10,}",  # Anthropic keys
    r"sk-(?:proj-)?[A-Za-z0-9_\-]{20,}",  # OpenAI-style keys
    r"AKIA[0-9A-Z]{16}",  # AWS access key ids
    r"(?i:bearer)\s+[A-Za-z0-9._\-]{20,}",  # bearer tokens
    r"gh[pousr]_[A-Za-z0-9]{30,}",  # GitHub tokens
]


class RedactionMiddleware(Middleware):
    """Masks secrets (API keys, tokens, custom patterns) before they reach a model or leave a tool."""

    def __init__(
        self, patterns: Sequence[str] = (), *, include_defaults: bool = True, replacement: str = "[REDACTED]"
    ) -> None:
        pats = [*(DEFAULT_SECRET_PATTERNS if include_defaults else []), *patterns]
        self._re = re.compile("|".join(f"(?:{p})" for p in pats)) if pats else None
        self.replacement = replacement
        self.redactions = 0

    def redact(self, text: str) -> str:
        if not self._re or not text:
            return text
        out, n = self._re.subn(self.replacement, text)
        self.redactions += n
        return out

    async def on_tool(self, inv: ToolInvocation, call_next: ToolNext) -> ToolResult:
        result = await call_next(inv)
        for block in result.content:
            if block.type == "text" and block.text:
                block.text = self.redact(block.text)
        return result

    async def on_llm(self, request: LLMRequest, call_next: LLMNext) -> LLMResponse:
        clean = request.model_copy(deep=True)
        clean.system = self.redact(clean.system)
        for m in clean.messages:
            self._redact_message(m)
        return await call_next(clean)

    def _redact_message(self, m: ChatMessage) -> None:
        if m.raw is not None:
            m.raw = json.loads(self.redact(json.dumps(m.raw)))
        for b in m.content:
            if isinstance(b, TextBlock):
                b.text = self.redact(b.text)
            elif isinstance(b, ToolResultBlock):
                for c in b.content:
                    if isinstance(c, TextBlock):
                        c.text = self.redact(c.text)


class GuardrailMiddleware(Middleware):
    """Refuses tool calls by name pattern, argument check or per-tool call count."""

    def __init__(
        self,
        *,
        deny: Sequence[str] = (),
        allow: Sequence[str] | None = None,
        check: Callable[[ToolInvocation], str | None] | None = None,
        max_calls: dict[str, int] | None = None,
    ) -> None:
        self.deny = list(deny)
        self.allow = list(allow) if allow is not None else None
        self.check = check
        self.max_calls = dict(max_calls or {})
        self.counts: dict[str, int] = {}

    async def on_tool(self, inv: ToolInvocation, call_next: ToolNext) -> ToolResult:
        name = inv.tool.name
        if any(fnmatch.fnmatch(name, p) for p in self.deny) or (
            self.allow is not None and not any(fnmatch.fnmatch(name, p) for p in self.allow)
        ):
            return ToolResult.error(
                "guardrail", f"{name} is not allowed here", "use another tool or ask the human", tool=name
            )
        limit = next((v for p, v in self.max_calls.items() if fnmatch.fnmatch(name, p)), None)
        if limit is not None:
            self.counts[name] = self.counts.get(name, 0) + 1
            if self.counts[name] > limit:
                return ToolResult.error(
                    "guardrail",
                    f"{name} was called more than {limit} times",
                    "you are probably looping: step back and try something else",
                    tool=name,
                )
        if self.check is not None:
            reason = self.check(inv)
            if reason:
                return ToolResult.error("guardrail", reason, tool=name)
        return await call_next(inv)


class CacheMiddleware(Middleware):
    """Reuses results of identical read-only tool calls; any mutating call clears the cache."""

    def __init__(self, ttl_seconds: float = 30.0, tools: Sequence[str] | None = None) -> None:
        self.ttl = ttl_seconds
        self.tools = list(tools) if tools is not None else None
        self._cache: dict[str, tuple[float, ToolResult]] = {}
        self.hits = 0

    async def on_tool(self, inv: ToolInvocation, call_next: ToolNext) -> ToolResult:
        if inv.tool.mutates:
            self._cache.clear()
            return await call_next(inv)
        if self.tools is not None and not any(fnmatch.fnmatch(inv.tool.name, p) for p in self.tools):
            return await call_next(inv)
        key = inv.tool.name + json.dumps(inv.args, sort_keys=True, default=str)
        hit = self._cache.get(key)
        now = time.monotonic()
        if hit is not None and now - hit[0] < self.ttl:
            self.hits += 1
            return hit[1].model_copy(deep=True)
        result = await call_next(inv)
        if not result.is_error:
            self._cache[key] = (now, result.model_copy(deep=True))
        return result
