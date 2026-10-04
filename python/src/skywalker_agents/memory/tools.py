"""Memory as agent tools: remember, recall, forget, summarize.

The same tools are served to every agent on the engine (``py_memory_*`` through the tool host) and
to Claude Code / Codex sessions (``sky-agents serve-mcp``). The caller's identity decides which
private and team memories it sees.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any, Literal

from ..engine.results import ToolResult
from ..tools.base import Tool, ToolContext

if TYPE_CHECKING:  # pragma: no cover
    from .memory import Memory

_SCOPE = {"type": "string", "enum": ["agent", "team", "project", "global"]}
_KIND = {"type": "string", "enum": ["episodic", "fact", "note", "artifact"]}
_STRS = {"type": "array", "items": {"type": "string"}}


def _reader(memory: Memory, ctx: ToolContext) -> Memory:
    if ctx.agent_id and ctx.agent_id != memory.agent:
        return memory.for_agent(ctx.agent_id, session=ctx.session)
    return memory


def memory_tools(memory: Memory, *, prefix: str = "memory_") -> list[Tool]:
    async def remember(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
        mem = _reader(memory, ctx)
        scope: Literal["agent", "team", "project", "global"] = args.get("scope") or (
            "agent" if mem.agent else "project"
        )
        try:
            item = await mem.remember(
                str(args["text"]),
                kind=args.get("kind", "note"),
                scope=scope,
                title=str(args.get("title", "")),
                tags=list(args.get("tags", [])),
                links=list(args.get("links", [])),
                importance=float(args.get("importance", 0.5)),
                team=args.get("team"),
                pin=bool(args.get("pin", False)),
            )
        except ValueError as e:
            return ToolResult.error("invalid_arguments", str(e), "scope agent needs an identity; team needs a team")
        return ToolResult.ok(f"remembered {item.id} ({item.kind}, {item.scope})", item.model_dump(mode="json"))

    async def recall(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
        mem = _reader(memory, ctx)
        hits = await mem.recall(
            str(args.get("query", "")),
            scopes=args.get("scopes"),
            kinds=args.get("kinds"),
            tags=args.get("tags"),
            k=int(args.get("k", 8)),
        )
        text = mem.context_block(hits, "Memories") or "(nothing relevant remembered)"
        return ToolResult.ok(
            text, {"hits": [{"score": round(h.score, 4), **h.item.model_dump(mode="json")} for h in hits]}
        )

    async def forget(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
        mem = _reader(memory, ctx)
        if not args.get("id") and not args.get("query"):
            return ToolResult.error("invalid_arguments", "forget needs an id or a query", "recall first to find the id")
        n = await mem.forget(
            args.get("id"), query=args.get("query"), scope=args.get("scope"), limit=int(args.get("limit", 1))
        )
        return ToolResult.ok(f"forgot {n} item(s)", {"forgotten": n})

    async def summarize(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
        mem = _reader(memory, ctx)
        text = await mem.summarize(
            str(args.get("query", "")),
            scopes=args.get("scopes"),
            kinds=args.get("kinds"),
            tags=args.get("tags"),
            limit=int(args.get("limit", 40)),
        )
        return ToolResult.ok(text, {"summary": text})

    return [
        Tool(
            f"{prefix}remember",
            "Remember something for later, for yourself or your team: a fact (durable: conventions, decisions, "
            "the human's preferences), a note (knowledge, can link other memories), or an episodic entry (what "
            "happened). scope: agent (private), team, project (everyone on this game) or global (the user, across "
            'projects). Example: {text: "The lava bridge must stay 3 m wide", kind: "fact", scope: "project", '
            'tags: ["level"]}.',
            {
                "type": "object",
                "properties": {
                    "text": {"type": "string", "description": "What to remember, self-contained"},
                    "kind": _KIND,
                    "scope": _SCOPE,
                    "title": {"type": "string"},
                    "tags": _STRS,
                    "links": {**_STRS, "description": "Related memory ids"},
                    "importance": {"type": "number", "description": "0..1 (default 0.5); >= 0.9 never decays"},
                    "team": {"type": "string", "description": "Team name for scope team"},
                    "pin": {"type": "boolean", "description": "Also add to your studio notes (shown in your brief)"},
                },
                "required": ["text"],
            },
            remember,
            mutates=True,
            category="memory",
        ),
        Tool(
            f"{prefix}recall",
            "Search what you, your team, the project and the user remember (keyword + meaning + recency + "
            "importance). Use it before starting work and when something seems familiar. Example: {query: "
            '"bridge width decisions", kinds: ["fact"]}.',
            {
                "type": "object",
                "properties": {
                    "query": {"type": "string"},
                    "scopes": {"type": "array", "items": _SCOPE},
                    "kinds": {"type": "array", "items": _KIND},
                    "tags": _STRS,
                    "k": {"type": "integer", "description": "Max results (default 8)"},
                },
            },
            recall,
            category="memory",
        ),
        Tool(
            f"{prefix}forget",
            "Delete an outdated or wrong memory, by id (from recall) or by query (deletes the best match).",
            {
                "type": "object",
                "properties": {
                    "id": {"type": "string"},
                    "query": {"type": "string"},
                    "scope": _SCOPE,
                    "limit": {"type": "integer"},
                },
            },
            forget,
            mutates=True,
            category="memory",
        ),
        Tool(
            f"{prefix}summarize",
            "A digest of what is remembered about a topic (grouped by kind and scope, with ids).",
            {
                "type": "object",
                "properties": {
                    "query": {"type": "string"},
                    "scopes": {"type": "array", "items": _SCOPE},
                    "kinds": {"type": "array", "items": _KIND},
                    "tags": _STRS,
                    "limit": {"type": "integer"},
                },
            },
            summarize,
            category="memory",
        ),
    ]
