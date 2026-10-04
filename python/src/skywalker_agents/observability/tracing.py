"""Tracing: spans for workflows, agent turns, LLM calls and tool calls.

Span attributes follow the OpenTelemetry GenAI semantic conventions (``gen_ai.*``), so the same
spans go to a JSONL file per run (``sky-agents trace show``), to any OpenTelemetry backend, and to
Langfuse or Phoenix (both ingest OTLP). Prompt and completion text is kept out of exported spans
unless ``capture_content=True``; the local JSONL keeps short previews.
"""

from __future__ import annotations

import contextlib
import contextvars
import json
import os
import secrets
import threading
import time
from collections.abc import Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Literal, Protocol

SpanKind = Literal["workflow", "step", "agent", "llm", "tool", "memory", "message", "custom"]

# GenAI semantic convention attribute names.
OPERATION = "gen_ai.operation.name"
PROVIDER = "gen_ai.provider.name"
SYSTEM = "gen_ai.system"
REQUEST_MODEL = "gen_ai.request.model"
RESPONSE_MODEL = "gen_ai.response.model"
MAX_TOKENS = "gen_ai.request.max_tokens"
FINISH_REASONS = "gen_ai.response.finish_reasons"
INPUT_TOKENS = "gen_ai.usage.input_tokens"
OUTPUT_TOKENS = "gen_ai.usage.output_tokens"
CACHE_READ_TOKENS = "gen_ai.usage.cache_read.input_tokens"
CACHE_WRITE_TOKENS = "gen_ai.usage.cache_creation.input_tokens"
AGENT_NAME = "gen_ai.agent.name"
AGENT_ID = "gen_ai.agent.id"
TOOL_NAME = "gen_ai.tool.name"
TOOL_CALL_ID = "gen_ai.tool.call.id"
TOOL_TYPE = "gen_ai.tool.type"
CONVERSATION_ID = "gen_ai.conversation.id"
COST = "skywalker.cost_usd"


@dataclass
class Span:
    name: str
    kind: SpanKind
    trace_id: str
    span_id: str
    parent_id: str | None
    start: float
    end: float | None = None
    attributes: dict[str, Any] = field(default_factory=dict)
    status: Literal["ok", "error"] = "ok"
    error: str = ""
    events: list[dict[str, Any]] = field(default_factory=list)

    def set(self, key: str, value: Any) -> Span:
        if value is not None:
            self.attributes[key] = value
        return self

    def update(self, values: dict[str, Any]) -> Span:
        for k, v in values.items():
            self.set(k, v)
        return self

    def event(self, name: str, **attrs: Any) -> None:
        self.events.append({"name": name, "time": time.time(), **attrs})

    def fail(self, error: BaseException | str) -> None:
        self.status = "error"
        self.error = error if isinstance(error, str) else f"{type(error).__name__}: {error}"

    @property
    def duration(self) -> float:
        return (self.end or time.time()) - self.start

    def to_json(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "kind": self.kind,
            "trace_id": self.trace_id,
            "span_id": self.span_id,
            "parent_id": self.parent_id,
            "start": self.start,
            "end": self.end,
            "duration": self.duration,
            "status": self.status,
            "error": self.error,
            "attributes": self.attributes,
            "events": self.events,
        }


class SpanSink(Protocol):
    def on_start(self, span: Span) -> None: ...

    def on_end(self, span: Span) -> None: ...


class MemorySink:
    """Keeps finished spans in memory (tests, evals)."""

    def __init__(self) -> None:
        self.spans: list[Span] = []

    def on_start(self, span: Span) -> None:
        pass

    def on_end(self, span: Span) -> None:
        self.spans.append(span)

    def named(self, prefix: str) -> list[Span]:
        return [s for s in self.spans if s.name.startswith(prefix)]


class JsonlSink:
    """One JSON line per finished span (the format ``sky-agents trace`` reads)."""

    def __init__(self, path: str | os.PathLike[str]) -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()

    def on_start(self, span: Span) -> None:
        pass

    def on_end(self, span: Span) -> None:
        line = json.dumps(span.to_json(), default=str, ensure_ascii=False)
        with self._lock, self.path.open("a", encoding="utf-8") as f:
            f.write(line + "\n")


_current: contextvars.ContextVar[Span | None] = contextvars.ContextVar("sky_agents_span", default=None)


class Tracer:
    """Creates spans and fans them out to sinks. Async-safe (context variables)."""

    def __init__(
        self, sinks: list[SpanSink] | None = None, *, capture_content: bool = False, preview_chars: int = 400
    ) -> None:
        self.sinks: list[SpanSink] = list(sinks or [])
        self.capture_content = capture_content
        self.preview_chars = preview_chars

    def add_sink(self, sink: SpanSink) -> Tracer:
        self.sinks.append(sink)
        return self

    @staticmethod
    def current() -> Span | None:
        return _current.get()

    @contextlib.contextmanager
    def span(self, name: str, kind: SpanKind = "custom", **attributes: Any) -> Iterator[Span]:
        parent = _current.get()
        span = Span(
            name=name,
            kind=kind,
            trace_id=parent.trace_id if parent else secrets.token_hex(16),
            span_id=secrets.token_hex(8),
            parent_id=parent.span_id if parent else None,
            start=time.time(),
        )
        span.update(attributes)
        for s in self.sinks:
            _safe(s.on_start, span)
        token = _current.set(span)
        try:
            yield span
        except BaseException as e:
            span.fail(e)
            raise
        finally:
            _current.reset(token)
            span.end = time.time()
            for s in self.sinks:
                _safe(s.on_end, span)

    def preview(self, text: str) -> str:
        return text if len(text) <= self.preview_chars else text[: self.preview_chars] + "…"


def _safe(fn: Any, span: Span) -> None:
    with contextlib.suppress(Exception):
        fn(span)


NULL_TRACER = Tracer()


def load_trace(path: str | os.PathLike[str]) -> list[dict[str, Any]]:
    """Reads a JSONL trace (skips malformed lines)."""
    spans = []
    with Path(path).open(encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                spans.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return spans
