"""The engine client: attach to the editor, spawn a headless engine, or use a fake."""

from .client import AgentSession, AsyncEngine, Engine, SyncAgentSession, ToolSpec
from .events import Event, EventStream
from .fake import FakeEngine
from .host import ToolHostServer
from .process import default_editor_socket, find_binary
from .results import ContentBlock, ToolResult

__all__ = [
    "AgentSession",
    "AsyncEngine",
    "ContentBlock",
    "Engine",
    "Event",
    "EventStream",
    "FakeEngine",
    "SyncAgentSession",
    "ToolHostServer",
    "ToolResult",
    "ToolSpec",
    "default_editor_socket",
    "find_binary",
]
