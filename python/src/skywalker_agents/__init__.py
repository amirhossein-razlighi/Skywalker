"""skywalker-agents: build, run and observe teams of AI agents on the Skywalker game engine.

The engine stays the source of truth (roster, board, feedback, messages, loops, the scene); this
package orchestrates: a typed engine client, agents with providers, tools, shared memory and
policies, communication, workflows with checkpoints and budgets, evals, tracing and plugins.

    from skywalker_agents import AsyncEngine, Agent, Memory

    async with await AsyncEngine.auto("examples/sky_dash") as engine:
        mira = Agent("mira", engine=engine, memory=Memory.open("examples/sky_dash"))
        result = await mira.run("Make the lava bridge fair")

See docs/PYTHON_AGENTS.md.
"""

from ._version import __version__
from .agents import Agent, AgentResult, Role
from .comms import Blackboard, Envelope, LocalBus, StudioBus
from .engine import AsyncEngine, Engine, Event, FakeEngine, ToolResult
from .errors import (
    ApprovalDenied,
    BudgetExceeded,
    EngineConnectionError,
    ProviderError,
    SkywalkerAgentsError,
    ToolError,
)
from .harness import (
    ApprovalPolicy,
    AutoApprove,
    AutoDeny,
    Budget,
    Recorder,
    Replayer,
    RunContext,
    RunResult,
    Scenario,
    StudioApprover,
    Workflow,
    load_workflow,
    run_eval,
    run_studio_loop,
)
from .hooks import CacheMiddleware, GuardrailMiddleware, LoggingMiddleware, Middleware, RedactionMiddleware
from .llm import ScriptedProvider, get_provider
from .memory import HashingEmbedder, Memory, MemoryItem, SQLiteMemoryStore
from .observability import CostMeter, JsonlSink, MemorySink, Tracer
from .plugins import registry
from .tools import Tool, ToolContext, Toolset, tool

__all__ = [
    "Agent",
    "AgentResult",
    "ApprovalDenied",
    "ApprovalPolicy",
    "AsyncEngine",
    "AutoApprove",
    "AutoDeny",
    "Blackboard",
    "Budget",
    "BudgetExceeded",
    "CacheMiddleware",
    "CostMeter",
    "Engine",
    "EngineConnectionError",
    "Envelope",
    "Event",
    "FakeEngine",
    "GuardrailMiddleware",
    "HashingEmbedder",
    "JsonlSink",
    "LocalBus",
    "LoggingMiddleware",
    "Memory",
    "MemoryItem",
    "MemorySink",
    "Middleware",
    "ProviderError",
    "Recorder",
    "RedactionMiddleware",
    "Replayer",
    "Role",
    "RunContext",
    "RunResult",
    "SQLiteMemoryStore",
    "Scenario",
    "ScriptedProvider",
    "SkywalkerAgentsError",
    "StudioApprover",
    "StudioBus",
    "Tool",
    "ToolContext",
    "ToolError",
    "ToolResult",
    "Toolset",
    "Tracer",
    "Workflow",
    "__version__",
    "get_provider",
    "load_workflow",
    "registry",
    "run_eval",
    "run_studio_loop",
    "tool",
]
