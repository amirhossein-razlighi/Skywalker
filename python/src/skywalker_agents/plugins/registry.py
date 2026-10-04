"""The extension registry: providers, memory stores, tools, workflow patterns, roles, hooks,
approvers and eval checks, built in or contributed by plugins.

A plugin is any installed distribution that declares an entry point in the
``skywalker_agents.plugins`` group pointing at a ``register(registry)`` function::

    # pyproject.toml of your plugin
    [project.entry-points."skywalker_agents.plugins"]
    my_plugin = "my_plugin:register"

    # my_plugin/__init__.py
    def register(registry):
        registry.tools.add("count_hazards", count_hazards)          # a Tool
        registry.providers.add("my-llm", lambda **kw: MyProvider(**kw))
        registry.patterns.add("pair_review", pair_review)           # a workflow pattern
        registry.hooks.add("audit", lambda **kw: AuditMiddleware(**kw))

``sky-agents new plugin NAME`` scaffolds one.
"""

from __future__ import annotations

import logging
from collections.abc import Callable, Iterator
from importlib import metadata
from typing import Any, Generic, TypeVar

from ..errors import PluginError

log = logging.getLogger("skywalker_agents.plugins")
T = TypeVar("T")
ENTRY_POINT_GROUP = "skywalker_agents.plugins"


class Registry(Generic[T]):
    """A named collection of one kind of extension."""

    def __init__(self, kind: str) -> None:
        self.kind = kind
        self._items: dict[str, T] = {}
        self._origin: dict[str, str] = {}

    def add(self, name: str, item: T, *, origin: str = "", replace: bool = False) -> T:
        if name in self._items and not replace:
            raise PluginError(
                f"{self.kind} '{name}' is already registered (by {self._origin.get(name) or 'builtin'}); "
                "pass replace=True to override it"
            )
        self._items[name] = item
        self._origin[name] = origin
        return item

    def get(self, name: str) -> T:
        try:
            return self._items[name]
        except KeyError:
            import difflib

            close = difflib.get_close_matches(name, list(self._items), n=1)
            hint = (
                f" (did you mean '{close[0]}'?)" if close else f" (known: {', '.join(sorted(self._items)) or 'none'})"
            )
            raise PluginError(f"no {self.kind} named '{name}'{hint}") from None

    def maybe(self, name: str) -> T | None:
        return self._items.get(name)

    def names(self) -> list[str]:
        return sorted(self._items)

    def origin(self, name: str) -> str:
        return self._origin.get(name, "")

    def __contains__(self, name: object) -> bool:
        return name in self._items

    def __iter__(self) -> Iterator[str]:
        return iter(sorted(self._items))


class PluginRegistry:
    """Everything that can be extended. ``registry`` (module level) is the process-wide instance."""

    def __init__(self) -> None:
        self.providers: Registry[Callable[..., Any]] = Registry("provider")
        self.memory_stores: Registry[Callable[..., Any]] = Registry("memory store")
        self.embedders: Registry[Callable[..., Any]] = Registry("embedder")
        self.tools: Registry[Any] = Registry("tool")
        self.patterns: Registry[Callable[..., Any]] = Registry("workflow pattern")
        self.roles: Registry[dict[str, Any]] = Registry("role")
        self.hooks: Registry[Callable[..., Any]] = Registry("hook")
        self.approvers: Registry[Callable[..., Any]] = Registry("approver")
        self.checks: Registry[Callable[..., Any]] = Registry("eval check")
        self.loaded: list[str] = []
        self.failed: dict[str, str] = {}
        self._plugins_loaded = False

    def load_plugins(self, *, force: bool = False) -> list[str]:
        """Runs every installed plugin's ``register(registry)`` once. Failures are logged, not raised."""
        if self._plugins_loaded and not force:
            return self.loaded
        self._plugins_loaded = True
        for ep in metadata.entry_points(group=ENTRY_POINT_GROUP):
            if ep.name in self.loaded:
                continue
            try:
                register = ep.load()
                register(self)
                self.loaded.append(ep.name)
            except Exception as e:
                self.failed[ep.name] = f"{type(e).__name__}: {e}"
                log.warning("plugin %s failed to load: %s", ep.name, e)
        return self.loaded

    def summary(self) -> dict[str, list[str]]:
        return {
            "providers": self.providers.names(),
            "memory_stores": self.memory_stores.names(),
            "embedders": self.embedders.names(),
            "tools": self.tools.names(),
            "patterns": self.patterns.names(),
            "roles": self.roles.names(),
            "hooks": self.hooks.names(),
            "approvers": self.approvers.names(),
            "checks": self.checks.names(),
            "plugins": list(self.loaded),
        }


registry = PluginRegistry()
