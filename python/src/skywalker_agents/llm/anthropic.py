"""Claude through the official Anthropic Python SDK (``pip install skywalker-agents[anthropic]``).

One :meth:`AnthropicProvider.complete` is one streamed Messages API request; the agent owns the
loop (budgets, approvals, hooks, tracing, replay). Defaults follow current guidance:

* model ``claude-opus-5-5`` (``claude-sonnet-5-5`` for cheaper roles), streaming with
  ``get_final_message()`` so long turns never hit HTTP timeouts, ``max_tokens`` 64000;
* adaptive thinking with an explicit ``output_config.effort`` (Opus 5.5 defaults to medium, and
  its thinking cannot be disabled, so effort is the only knob);
* prompt caching: a breakpoint on the frozen system prompt (tools + system) and top-level automatic
  caching for the growing history;
* server-side refusal fallback (``fallbacks: "default"``) on models that support it, Claude API only;
* ``eager_input_streaming`` on tools; tool inputs are validated by the tools before they run, a turn
  cut off by ``max_tokens`` never runs its tools, a ``refusal`` discards the partial output, and
  ``pause_turn`` is resumed by the agent;
* append-only history: assistant content (thinking blocks included) is echoed back unchanged.
"""

from __future__ import annotations

import asyncio
import os
from typing import Any

from ..errors import ProviderError
from .types import (
    ChatMessage,
    ImageBlock,
    LLMRequest,
    LLMResponse,
    StopReason,
    TextBlock,
    ToolCall,
    ToolResultBlock,
    Usage,
)

DEFAULT_MODEL = "claude-opus-5-5"
CHEAP_MODEL = "claude-sonnet-5-5"
_FALLBACK_BETA = "server-side-fallback-2026-07-01"
# Models that accept `fallbacks: "default"` on the Claude API.
_FALLBACK_MODELS = ("claude-opus-5-5", "claude-opus-5", "claude-sonnet-5-5", "claude-fable-5-1", "claude-fable-5")
# Models with adaptive thinking and effort (older ones and Haiku get neither).
_ADAPTIVE = (
    "claude-fable",
    "claude-mythos",
    "claude-opus-5",
    "claude-opus-4-8",
    "claude-opus-4-7",
    "claude-opus-4-6",
    "claude-sonnet-5",
    "claude-sonnet-4-6",
)


def _supports(model: str, prefixes: tuple[str, ...]) -> bool:
    return any(model == p or model.startswith(p) for p in prefixes)


class AnthropicProvider:
    """Claude via ``anthropic.AsyncAnthropic`` (credentials from the environment or ``ant auth login``)."""

    name = "anthropic"
    supports_vision = True

    def __init__(
        self,
        client: Any | None = None,
        *,
        default_model: str = DEFAULT_MODEL,
        fallbacks: bool = True,
        cache: bool = True,
        eager_input_streaming: bool = True,
        max_retries: int = 2,
        timeout: float | None = None,
        json_retries: int = 2,
    ) -> None:
        if client is None:
            try:
                import anthropic
            except ImportError as e:  # pragma: no cover - depends on the extra
                raise ProviderError("the Anthropic provider needs `pip install skywalker-agents[anthropic]`") from e
            kwargs: dict[str, Any] = {"max_retries": max_retries}
            if timeout is not None:
                kwargs["timeout"] = timeout
            client = anthropic.AsyncAnthropic(**kwargs)
        self.client = client
        self.default_model = default_model
        self.fallbacks = fallbacks
        self.cache = cache
        self.eager_input_streaming = eager_input_streaming
        self.json_retries = json_retries

    # ------------------------------------------------------------------ request building
    def _first_party(self) -> bool:
        base = str(getattr(self.client, "base_url", "") or os.environ.get("ANTHROPIC_BASE_URL", ""))
        return not base or "api.anthropic.com" in base

    def build_params(self, req: LLMRequest) -> dict[str, Any]:
        model = req.model or self.default_model
        params: dict[str, Any] = {
            "model": model,
            "max_tokens": req.max_tokens,
            "messages": [m for m in (self._message(x) for x in req.messages) if m["content"]],
        }
        if req.system:
            block: dict[str, Any] = {"type": "text", "text": req.system}
            if self.cache:
                block["cache_control"] = {"type": "ephemeral"}
            params["system"] = [block]
        if req.tools:
            tools = []
            for t in req.tools:
                spec: dict[str, Any] = {"name": t.name, "description": t.description, "input_schema": t.input_schema}
                if self.eager_input_streaming:
                    spec["eager_input_streaming"] = True
                tools.append(spec)
            params["tools"] = tools
        if self.cache:
            params["cache_control"] = {"type": "ephemeral"}  # automatic caching of the growing history
        if _supports(model, _ADAPTIVE):
            params["thinking"] = {"type": "adaptive"}
            if req.effort:
                params["output_config"] = {"effort": req.effort}
        betas: list[str] = []
        if self.fallbacks and _supports(model, _FALLBACK_MODELS) and self._first_party():
            betas.append(_FALLBACK_BETA)
            params["fallbacks"] = "default"
        if betas:
            params["betas"] = betas
        return params

    @staticmethod
    def _message(m: ChatMessage) -> dict[str, Any]:
        if m.role == "assistant":
            if m.raw is not None and m.provider == "anthropic":
                return {"role": "assistant", "content": m.raw}  # unchanged: thinking blocks and signatures
            content: list[dict[str, Any]] = []
            for b in m.content:
                if isinstance(b, TextBlock) and b.text:
                    content.append({"type": "text", "text": b.text})
                elif isinstance(b, ToolCall):
                    content.append({"type": "tool_use", "id": b.id, "name": b.name, "input": b.input})
            return {"role": "assistant", "content": content}
        content = []
        results = [b for b in m.content if isinstance(b, ToolResultBlock)]
        for r in results:  # tool results first, all in one user message
            inner: list[dict[str, Any]] = []
            for c in r.content:
                if isinstance(c, TextBlock):
                    inner.append({"type": "text", "text": c.text or "(empty)"})
                else:
                    inner.append(
                        {"type": "image", "source": {"type": "base64", "media_type": c.media_type, "data": c.data}}
                    )
            block: dict[str, Any] = {
                "type": "tool_result",
                "tool_use_id": r.tool_call_id,
                "content": inner or [{"type": "text", "text": "(empty)"}],
            }
            if r.is_error:
                block["is_error"] = True
            content.append(block)
        for b in m.content:
            if isinstance(b, TextBlock) and b.text:
                text = f"[operator note] {b.text}" if m.role == "system" else b.text
                content.append({"type": "text", "text": text})
            elif isinstance(b, ImageBlock):
                content.append(
                    {"type": "image", "source": {"type": "base64", "media_type": b.media_type, "data": b.data}}
                )
        return {"role": "user", "content": content}

    # ------------------------------------------------------------------ the call
    async def complete(self, request: LLMRequest) -> LLMResponse:
        import anthropic

        params = self.build_params(request)
        attempts = 0
        while True:
            try:
                async with self.client.beta.messages.stream(**params) as stream:
                    message = await stream.get_final_message()
                break
            except ValueError as e:
                # Eager input streaming: a tool input that cannot be parsed at all. We do not hold the
                # block, so re-issue the request (bounded).
                attempts += 1
                if attempts > self.json_retries:
                    raise ProviderError(f"Claude produced unparseable tool input {attempts} times: {e}") from e
                await asyncio.sleep(0.5 * attempts)
            except anthropic.BadRequestError as e:
                raise ProviderError(f"anthropic rejected the request: {e.message}", retryable=False) from e
            except (anthropic.AuthenticationError, anthropic.PermissionDeniedError) as e:
                raise ProviderError(f"anthropic credentials: {e.message}", retryable=False) from e
            except anthropic.NotFoundError as e:
                raise ProviderError(
                    f"anthropic: unknown model or endpoint ({params['model']}): {e.message}", retryable=False
                ) from e
            except anthropic.RateLimitError as e:
                raise ProviderError(f"anthropic rate limit (after SDK retries): {e.message}") from e
            except anthropic.APIStatusError as e:
                raise ProviderError(f"anthropic API error {e.status_code}: {e.message}") from e
            except anthropic.APIConnectionError as e:
                raise ProviderError(f"cannot reach the Anthropic API: {e}") from e
        return self.parse(message, params["model"])

    @staticmethod
    def parse(message: Any, model: str) -> LLMResponse:
        stop: StopReason = message.stop_reason or "end_turn"
        if stop not in ("end_turn", "tool_use", "max_tokens", "refusal", "pause_turn", "stop_sequence"):
            stop = "end_turn"
        raw: list[dict[str, Any]] = []
        blocks: list[Any] = []
        for b in message.content:
            raw.append(b.model_dump(mode="json", exclude_none=True))
            if b.type == "text":
                blocks.append(TextBlock(text=b.text))
            elif b.type == "tool_use":
                ok = isinstance(b.input, dict)
                blocks.append(ToolCall(id=b.id, name=b.name, input=b.input if ok else {}, invalid_json=not ok))
        u = message.usage
        usage = Usage(
            input_tokens=getattr(u, "input_tokens", 0) or 0,
            output_tokens=getattr(u, "output_tokens", 0) or 0,
            cache_read_tokens=getattr(u, "cache_read_input_tokens", 0) or 0,
            cache_write_tokens=getattr(u, "cache_creation_input_tokens", 0) or 0,
            requests=1,
        )
        category = None
        if stop == "refusal":
            details = getattr(message, "stop_details", None)
            category = getattr(details, "category", None) if details is not None else None
            blocks, raw = [], []  # a refused turn's partial output is discarded and its tools never run
        return LLMResponse(
            content=blocks,
            stop_reason=stop,
            usage=usage,
            model=getattr(message, "model", model) or model,
            provider="anthropic",
            raw=raw or None,
            refusal_category=category,
        )
