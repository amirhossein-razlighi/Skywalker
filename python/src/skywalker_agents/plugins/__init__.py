"""Extensibility: the plugin registry (entry point group ``skywalker_agents.plugins``) and scaffolding."""

from .builtins import ensure_builtins, register_builtins
from .registry import ENTRY_POINT_GROUP, PluginRegistry, Registry, registry

__all__ = ["ENTRY_POINT_GROUP", "PluginRegistry", "Registry", "ensure_builtins", "register_builtins", "registry"]
