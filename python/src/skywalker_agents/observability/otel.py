"""OpenTelemetry export (``pip install skywalker-agents[otel]``), including Langfuse and Phoenix.

tracer = Tracer([OTelSink(configure_otel(endpoint="http://localhost:4318/v1/traces"))])
tracer = Tracer([OTelSink(langfuse())])          # LANGFUSE_PUBLIC_KEY / LANGFUSE_SECRET_KEY / LANGFUSE_HOST
tracer = Tracer([OTelSink(phoenix())])           # PHOENIX_COLLECTOR_ENDPOINT or localhost:6006
"""

from __future__ import annotations

import base64
import os
from typing import Any

from .tracing import Span

_KIND_OPERATION = {"agent": "invoke_agent", "llm": "chat", "tool": "execute_tool"}
_CONTENT_KEYS = ("skywalker.prompt", "skywalker.completion", "skywalker.tool.args", "skywalker.tool.result")


def configure_otel(
    endpoint: str | None = None, headers: dict[str, str] | None = None, service_name: str = "sky-agents"
) -> Any:
    """A TracerProvider exporting OTLP/HTTP to ``endpoint`` (default: OTEL_EXPORTER_OTLP_TRACES_ENDPOINT)."""
    from opentelemetry.exporter.otlp.proto.http.trace_exporter import OTLPSpanExporter
    from opentelemetry.sdk.resources import Resource
    from opentelemetry.sdk.trace import TracerProvider
    from opentelemetry.sdk.trace.export import BatchSpanProcessor

    provider = TracerProvider(resource=Resource.create({"service.name": service_name}))
    exporter = OTLPSpanExporter(endpoint=endpoint, headers=headers) if endpoint else OTLPSpanExporter(headers=headers)
    provider.add_span_processor(BatchSpanProcessor(exporter))
    return provider


def langfuse(public_key: str | None = None, secret_key: str | None = None, host: str | None = None) -> Any:
    """A TracerProvider that sends to Langfuse's OTLP endpoint (keys from LANGFUSE_* env vars)."""
    pk = public_key or os.environ["LANGFUSE_PUBLIC_KEY"]
    sk = secret_key or os.environ["LANGFUSE_SECRET_KEY"]
    base = (host or os.environ.get("LANGFUSE_HOST", "https://cloud.langfuse.com")).rstrip("/")
    auth = base64.b64encode(f"{pk}:{sk}".encode()).decode()
    return configure_otel(f"{base}/api/public/otel/v1/traces", {"Authorization": f"Basic {auth}"})


def phoenix(endpoint: str | None = None) -> Any:
    """A TracerProvider that sends to Arize Phoenix (default http://localhost:6006/v1/traces)."""
    url = endpoint or os.environ.get("PHOENIX_COLLECTOR_ENDPOINT", "http://localhost:6006")
    if not url.rstrip("/").endswith("/v1/traces"):
        url = url.rstrip("/") + "/v1/traces"
    return configure_otel(url)


class OTelSink:
    """Mirrors spans into OpenTelemetry (parents before children, GenAI attribute names)."""

    def __init__(self, tracer_provider: Any | None = None, *, capture_content: bool = False) -> None:
        from opentelemetry import trace

        provider = tracer_provider or trace.get_tracer_provider()
        self._tracer = provider.get_tracer("skywalker_agents")
        self._provider = provider
        self._open: dict[str, Any] = {}
        self.capture_content = capture_content

    def on_start(self, span: Span) -> None:
        from opentelemetry import trace

        parent = self._open.get(span.parent_id or "")
        ctx = trace.set_span_in_context(parent) if parent is not None else None
        kind = trace.SpanKind.CLIENT if span.kind in ("llm", "tool") else trace.SpanKind.INTERNAL
        otel_span = self._tracer.start_span(span.name, context=ctx, kind=kind, start_time=int(span.start * 1e9))
        self._open[span.span_id] = otel_span

    def on_end(self, span: Span) -> None:
        from opentelemetry.trace import Status, StatusCode

        otel_span = self._open.pop(span.span_id, None)
        if otel_span is None:
            return
        if span.kind in _KIND_OPERATION:
            otel_span.set_attribute("gen_ai.operation.name", _KIND_OPERATION[span.kind])
        for k, v in span.attributes.items():
            if not self.capture_content and k in _CONTENT_KEYS:
                continue
            otel_span.set_attribute(k, _otel_value(v))
        for ev in span.events:
            otel_span.add_event(
                str(ev.get("name", "event")), {k: _otel_value(v) for k, v in ev.items() if k not in ("name", "time")}
            )
        if span.status == "error":
            otel_span.set_status(Status(StatusCode.ERROR, span.error))
        otel_span.end(end_time=int((span.end or span.start) * 1e9))

    def flush(self) -> None:
        flush = getattr(self._provider, "force_flush", None)
        if flush is not None:
            flush()


def _otel_value(v: Any) -> Any:
    if isinstance(v, (str, bool, int, float)):
        return v
    if isinstance(v, (list, tuple)) and all(isinstance(x, (str, bool, int, float)) for x in v):
        return list(v)
    import json

    return json.dumps(v, default=str)
