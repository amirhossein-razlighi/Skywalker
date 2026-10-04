"""Built-in registrations (lazy, so optional dependencies are imported only when used)."""

from __future__ import annotations

from typing import Any

from .registry import PluginRegistry, registry

_done = False


def register_builtins(reg: PluginRegistry) -> None:
    def anthropic(**kw: Any) -> Any:
        from ..llm.anthropic import AnthropicProvider

        return AnthropicProvider(**kw)

    def openai_compatible(preset: str) -> Any:
        def make(**kw: Any) -> Any:
            from ..llm.openai_compat import OpenAICompatibleProvider

            return OpenAICompatibleProvider(preset, **kw)

        return make

    def litellm(**kw: Any) -> Any:
        from ..llm.litellm_provider import LiteLLMProvider

        return LiteLLMProvider(**kw)

    def scripted(**kw: Any) -> Any:
        from ..llm.scripted import ScriptedProvider

        if "path" in kw:
            return ScriptedProvider.from_file(kw.pop("path"))
        return ScriptedProvider(**kw)

    def echo(**kw: Any) -> Any:
        from ..llm.scripted import EchoProvider

        return EchoProvider()

    reg.providers.add("anthropic", anthropic, origin="builtin")
    for preset in ("openai", "ollama", "lmstudio", "vllm", "llamacpp"):
        reg.providers.add(preset, openai_compatible(preset), origin="builtin")
    reg.providers.add("litellm", litellm, origin="builtin")
    reg.providers.add("scripted", scripted, origin="builtin")
    reg.providers.add("mock", scripted, origin="builtin")
    reg.providers.add("echo", echo, origin="builtin")

    def sqlite_store(**kw: Any) -> Any:
        from ..memory.sqlite import SQLiteMemoryStore

        return SQLiteMemoryStore(**kw)

    def qdrant_store(**kw: Any) -> Any:
        from ..memory.qdrant import QdrantMemoryStore

        return QdrantMemoryStore(**kw)

    reg.memory_stores.add("sqlite", sqlite_store, origin="builtin")
    reg.memory_stores.add("qdrant", qdrant_store, origin="builtin")

    def hashing(**kw: Any) -> Any:
        from ..memory.embeddings import HashingEmbedder

        return HashingEmbedder(**kw)

    def st(**kw: Any) -> Any:
        from ..memory.embeddings import SentenceTransformerEmbedder

        return SentenceTransformerEmbedder(**kw)

    def oai_embed(**kw: Any) -> Any:
        from ..memory.embeddings import OpenAIEmbedder

        return OpenAIEmbedder(**kw)

    reg.embedders.add("hashing", hashing, origin="builtin")
    reg.embedders.add("sentence-transformers", st, origin="builtin")
    reg.embedders.add("openai", oai_embed, origin="builtin")

    from ..harness.patterns import PATTERNS

    for name, fn in PATTERNS.items():
        reg.patterns.add(name, fn, origin="builtin")

    from .. import hooks

    reg.hooks.add("logging", hooks.LoggingMiddleware, origin="builtin")
    reg.hooks.add("redaction", hooks.RedactionMiddleware, origin="builtin")
    reg.hooks.add("guardrail", hooks.GuardrailMiddleware, origin="builtin")
    reg.hooks.add("cache", hooks.CacheMiddleware, origin="builtin")

    from ..harness import approvals

    reg.approvers.add("auto", approvals.AutoApprove, origin="builtin")
    reg.approvers.add("deny", approvals.AutoDeny, origin="builtin")
    reg.approvers.add("terminal", approvals.TerminalApprover, origin="builtin")
    reg.approvers.add("studio", approvals.StudioApprover, origin="builtin")

    from ..harness import evals

    reg.checks.add("tool", evals.tool_check, origin="builtin")
    reg.checks.add("sim_trace", evals.sim_trace_check, origin="builtin")
    reg.checks.add("metric", evals.metric_check, origin="builtin")
    reg.checks.add("vision", evals.vision_check, origin="builtin")
    reg.checks.add("text", evals.text_check, origin="builtin")


def ensure_builtins(load_plugins: bool = True) -> PluginRegistry:
    """Registers the built-ins once, then installed plugins (entry points)."""
    global _done
    if not _done:
        _done = True
        register_builtins(registry)
        if load_plugins:
            registry.load_plugins()
    return registry
