"""Exceptions raised by the agent layer.

Engine errors keep the engine's stable ``code`` (``not_found``, ``invalid_arguments``...) and its
``hint`` so callers (and models) can react to them precisely.
"""

from __future__ import annotations


class SkywalkerAgentsError(Exception):
    """Base class for every error raised by skywalker_agents."""


class EngineConnectionError(SkywalkerAgentsError):
    """The engine could not be reached, or the connection broke."""


class EngineNotFoundError(EngineConnectionError):
    """No ``skywalker`` binary was found (set SKYWALKER_BIN or put it on PATH)."""


class ProtocolError(EngineConnectionError):
    """The engine answered with a JSON-RPC error (unknown tool, bad request...)."""

    def __init__(self, message: str, code: int | None = None) -> None:
        super().__init__(message)
        self.rpc_code = code


class ToolError(SkywalkerAgentsError):
    """A tool ran and reported an error (``isError`` in its result)."""

    def __init__(self, tool: str, code: str, message: str, hint: str = "") -> None:
        text = f"{tool}: [{code}] {message}"
        if hint:
            text += f" (hint: {hint})"
        super().__init__(text)
        self.tool = tool
        self.code = code
        self.message = message
        self.hint = hint


class BudgetExceeded(SkywalkerAgentsError):
    """A token, cost, time or tool-call budget ran out."""

    def __init__(self, scope: str, resource: str, used: float, limit: float) -> None:
        super().__init__(f"{scope}: {resource} budget exceeded ({used:g} > {limit:g})")
        self.scope = scope
        self.resource = resource
        self.used = used
        self.limit = limit


class ApprovalDenied(SkywalkerAgentsError):
    """A human (or policy) declined an action that needed approval."""


class ReplayMismatch(SkywalkerAgentsError):
    """A strict replay met a request that was not recorded."""


class ProviderError(SkywalkerAgentsError):
    """A model provider failed after its own retries. ``retryable`` is False for requests that can
    never succeed as sent (bad request, credentials, unknown model)."""

    def __init__(self, message: str, *, retryable: bool = True) -> None:
        super().__init__(message)
        self.retryable = retryable


class PluginError(SkywalkerAgentsError):
    """A plugin could not be loaded or registered."""
