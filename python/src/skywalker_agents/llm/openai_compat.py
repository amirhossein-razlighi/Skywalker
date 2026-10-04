"""Any OpenAI-compatible Chat Completions endpoint through the official ``openai`` SDK.

Covers OpenAI itself and local servers: Ollama, LM Studio, vLLM, llama.cpp, plus hosted
compatible APIs (OpenRouter, Groq, DeepSeek, ...). ``pip install skywalker-agents[openai]``.
Keys come from the environment (``OPENAI_API_KEY``); local servers need none.
"""

from __future__ import annotations

import json
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

PRESETS: dict[str, dict[str, str]] = {
    "openai": {"base_url": "", "api_key_env": "OPENAI_API_KEY"},
    "ollama": {"base_url": "http://localhost:11434/v1", "api_key": "ollama"},
    "lmstudio": {"base_url": "http://localhost:1234/v1", "api_key": "lm-studio"},
    "vllm": {"base_url": "http://localhost:8000/v1", "api_key": "EMPTY"},
    "llamacpp": {"base_url": "http://localhost:8080/v1", "api_key": "EMPTY"},
}

_FINISH: dict[str, StopReason] = {
    "stop": "end_turn",
    "tool_calls": "tool_use",
    "function_call": "tool_use",
    "length": "max_tokens",
    "content_filter": "refusal",
}


class OpenAICompatibleProvider:
    """Chat Completions with function tools. Screenshots in tool results follow as an image user message."""

    supports_vision: bool

    def __init__(
        self,
        preset: str = "openai",
        *,
        client: Any | None = None,
        base_url: str | None = None,
        api_key: str | None = None,
        default_model: str | None = None,
        vision: bool | None = None,
        max_tokens_cap: int = 8192,
        max_retries: int = 2,
    ) -> None:
        self.name = preset
        conf = PRESETS.get(preset, {})
        base = (
            base_url
            or os.environ.get(f"{preset.upper()}_BASE_URL")
            or (os.environ.get("OPENAI_BASE_URL") if preset == "openai" else None)
            or conf.get("base_url")
            or None
        )
        if client is None:
            try:
                import openai
            except ImportError as e:  # pragma: no cover - depends on the extra
                raise ProviderError("OpenAI-compatible providers need `pip install skywalker-agents[openai]`") from e
            key = api_key or os.environ.get(conf.get("api_key_env", "OPENAI_API_KEY")) or conf.get("api_key") or "EMPTY"
            client = openai.AsyncOpenAI(base_url=base, api_key=key, max_retries=max_retries)
        self.client = client
        self.default_model = (
            default_model or os.environ.get(f"{preset.upper()}_MODEL") or os.environ.get("OPENAI_MODEL", "")
        )
        if vision is None:
            vision = os.environ.get("OPENAI_VISION", "1") != "0"
        self.supports_vision = vision
        self.max_tokens_cap = max_tokens_cap
        self._official = preset == "openai" and not base

    # ------------------------------------------------------------------ request building
    def build_params(self, req: LLMRequest) -> dict[str, Any]:
        model = req.model or self.default_model
        if not model:
            raise ProviderError(
                f"no model for provider '{self.name}': pass model=... or set {self.name.upper()}_MODEL / OPENAI_MODEL"
            )
        messages: list[dict[str, Any]] = []
        if req.system:
            messages.append({"role": "system", "content": req.system})
        for m in req.messages:
            messages.extend(self._messages(m))
        params: dict[str, Any] = {"model": model, "messages": messages}
        limit = min(req.max_tokens, self.max_tokens_cap)
        params["max_completion_tokens" if self._official else "max_tokens"] = limit
        if req.tools:
            params["tools"] = [
                {
                    "type": "function",
                    "function": {"name": t.name, "description": t.description, "parameters": t.input_schema},
                }
                for t in req.tools
            ]
        return params

    def _image_part(self, b: ImageBlock) -> dict[str, Any]:
        return {"type": "image_url", "image_url": {"url": f"data:{b.media_type};base64,{b.data}"}}

    def _messages(self, m: ChatMessage) -> list[dict[str, Any]]:
        if m.role == "assistant":
            if m.raw is not None and m.provider == self.name and len(m.raw) == 1:
                return [m.raw[0]]
            calls = [
                {
                    "id": c.id,
                    "type": "function",
                    "function": {"name": c.name, "arguments": c.raw_input or json.dumps(c.input)},
                }
                for c in m.tool_calls
            ]
            out: dict[str, Any] = {"role": "assistant", "content": m.text or None}
            if calls:
                out["tool_calls"] = calls
            return [out]
        result: list[dict[str, Any]] = []
        followup_images: list[dict[str, Any]] = []
        for b in m.content:
            if isinstance(b, ToolResultBlock):
                text = b.text or "(empty)"
                if b.is_error:
                    text = f"ERROR: {text}"
                result.append({"role": "tool", "tool_call_id": b.tool_call_id, "content": text})
                followup_images += [self._image_part(i) for i in b.content if isinstance(i, ImageBlock)]
        parts: list[dict[str, Any]] = []
        for b in m.content:
            if isinstance(b, TextBlock) and b.text:
                parts.append({"type": "text", "text": b.text})
            elif isinstance(b, ImageBlock) and self.supports_vision:
                parts.append(self._image_part(b))
        if followup_images and self.supports_vision:
            parts = [{"type": "text", "text": "Images returned by the tools above:"}, *followup_images, *parts]
        if parts:
            role = "system" if m.role == "system" else "user"
            if all(p["type"] == "text" for p in parts):
                result.append({"role": role, "content": "\n".join(p["text"] for p in parts)})
            else:
                result.append({"role": "user", "content": parts})
        return result

    # ------------------------------------------------------------------ the call
    async def complete(self, request: LLMRequest) -> LLMResponse:
        import openai

        params = self.build_params(request)
        try:
            resp = await self.client.chat.completions.create(**params)
        except openai.BadRequestError as e:
            raise ProviderError(f"{self.name} rejected the request: {e}", retryable=False) from e
        except openai.AuthenticationError as e:
            raise ProviderError(f"{self.name} credentials: {e}", retryable=False) from e
        except openai.RateLimitError as e:
            raise ProviderError(f"{self.name} rate limit (after retries): {e}") from e
        except openai.APIStatusError as e:
            raise ProviderError(f"{self.name} API error {e.status_code}: {e}") from e
        except openai.APIConnectionError as e:
            raise ProviderError(f"cannot reach {self.name} at {self.client.base_url}: {e}") from e
        return self.parse(resp, params["model"])

    def parse(self, resp: Any, model: str) -> LLMResponse:
        choice = resp.choices[0]
        msg = choice.message
        blocks: list[Any] = []
        if msg.content:
            blocks.append(TextBlock(text=msg.content))
        for c in msg.tool_calls or []:
            raw_args = c.function.arguments or "{}"
            try:
                parsed = json.loads(raw_args)
                ok = isinstance(parsed, dict)
            except json.JSONDecodeError:
                parsed, ok = {}, False
            blocks.append(
                ToolCall(
                    id=c.id, name=c.function.name, input=parsed if ok else {}, invalid_json=not ok, raw_input=raw_args
                )
            )
        stop = _FINISH.get(choice.finish_reason or "stop", "end_turn")
        if stop == "end_turn" and any(isinstance(b, ToolCall) for b in blocks):
            stop = "tool_use"
        u = resp.usage
        cached = 0
        if u is not None and getattr(u, "prompt_tokens_details", None) is not None:
            cached = getattr(u.prompt_tokens_details, "cached_tokens", 0) or 0
        usage = Usage(
            input_tokens=max(0, (u.prompt_tokens if u else 0) - cached),
            output_tokens=u.completion_tokens if u else 0,
            cache_read_tokens=cached,
            requests=1,
        )
        raw_msg = msg.model_dump(mode="json", exclude_none=True)
        raw_msg = {k: v for k, v in raw_msg.items() if k in ("role", "content", "tool_calls")}
        raw_msg.setdefault("content", None)
        return LLMResponse(
            content=blocks,
            stop_reason=stop,
            usage=usage,
            model=getattr(resp, "model", model) or model,
            provider=self.name,
            raw=[raw_msg],
        )
