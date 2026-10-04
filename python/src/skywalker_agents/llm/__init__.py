"""Model providers behind one small interface (:class:`Provider`)."""

from __future__ import annotations

from typing import Any

from .pricing import cost_usd, price_for, set_price
from .scripted import EchoProvider, ScriptedProvider
from .types import (
    Block,
    ChatMessage,
    ImageBlock,
    LLMRequest,
    LLMResponse,
    Provider,
    TextBlock,
    ToolCall,
    ToolResultBlock,
    ToolSpecParam,
    Usage,
)

DEFAULT_MODEL = "claude-opus-5-5"
CHEAP_MODEL = "claude-sonnet-5-5"


def get_provider(spec: str | Provider = "anthropic", **kwargs: Any) -> Provider:
    """A provider by name: ``anthropic``, ``openai``, ``ollama``, ``lmstudio``, ``vllm``, ``llamacpp``,
    ``litellm``, ``scripted``/``mock``, ``echo``, a plugin's name, or any other name as an
    OpenAI-compatible endpoint configured by ``<NAME>_BASE_URL`` / ``<NAME>_API_KEY``."""
    if not isinstance(spec, str):
        return spec
    from ..plugins.builtins import ensure_builtins
    from ..plugins.registry import registry

    ensure_builtins()
    factory = registry.providers.maybe(spec)
    if factory is not None:
        provider: Provider = factory(**kwargs)
        return provider
    from .openai_compat import OpenAICompatibleProvider

    return OpenAICompatibleProvider(spec, **kwargs)


__all__ = [
    "CHEAP_MODEL",
    "DEFAULT_MODEL",
    "Block",
    "ChatMessage",
    "EchoProvider",
    "ImageBlock",
    "LLMRequest",
    "LLMResponse",
    "Provider",
    "ScriptedProvider",
    "TextBlock",
    "ToolCall",
    "ToolResultBlock",
    "ToolSpecParam",
    "Usage",
    "cost_usd",
    "get_provider",
    "price_for",
    "set_price",
]
