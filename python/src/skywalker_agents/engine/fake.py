"""An in-process stand-in for the engine, for unit tests and offline development.

It speaks the same tool protocol as the real engine (results, errors with codes and hints,
actors, the event stream, tool hosting) and keeps a small studio: roster, messages and threads,
board, feedback and decisions, presence, usage, memory notes and a simplified loop state
machine. Scene tools are minimal (entities with names and positions; captures are tiny PNGs).

Extend it per test with :meth:`FakeEngine.add_tool`.
"""

from __future__ import annotations

import asyncio
import base64
import itertools
import re
import time
from collections.abc import Awaitable, Callable
from dataclasses import dataclass, field
from typing import Any

from .results import ToolResult

Handler = Callable[[dict[str, Any], str], "ToolResult | Awaitable[ToolResult]"]

# A 1x1 PNG (opaque sky blue): what fake captures return.
TINY_PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADUlEQVR4nGNo2HcfAAQaAk0tTuMQAAAAAElFTkSuQmCC"
)


@dataclass
class FakeTool:
    name: str
    description: str
    schema: dict[str, Any]
    handler: Handler
    mutates: bool = False
    category: str = "fake"

    def spec(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "title": self.name,
            "description": self.description,
            "inputSchema": self.schema,
            "annotations": {"readOnlyHint": not self.mutates},
            "_meta": {"skywalker/category": self.category},
        }


def _obj(props: dict[str, Any] | None = None, required: list[str] | None = None) -> dict[str, Any]:
    out: dict[str, Any] = {"type": "object", "properties": props or {}}
    if required:
        out["required"] = required
    return out


S = {"type": "string"}
I = {"type": "integer"}  # noqa: E741
B = {"type": "boolean"}
O = {"type": "object"}  # noqa: E741
A = {"type": "array"}


@dataclass
class _Host:
    id: str
    tools: list[str]
    queue: list[dict[str, Any]] = field(default_factory=list)


class FakeEngine:
    """Shared state behind every :class:`FakeConnection`."""

    def __init__(self, *, roster: list[dict[str, Any]] | None = None) -> None:
        self.tools: dict[str, FakeTool] = {}
        self.events: list[dict[str, Any]] = []
        self._seq = 0
        self._changed: asyncio.Condition | None = None
        self._cond_loop: asyncio.AbstractEventLoop | None = None
        self._tasks: set[asyncio.Future[Any]] = set()
        self.agents: dict[str, dict[str, Any]] = {}
        self.messages: list[dict[str, Any]] = []
        self.tasks: list[dict[str, Any]] = []
        self.feedback: list[dict[str, Any]] = []
        self.decisions: list[dict[str, Any]] = []
        self.presence: dict[str, dict[str, Any]] = {}
        self.usage: dict[str, dict[str, Any]] = {}
        self.entities: list[dict[str, Any]] = []
        self.loops: dict[str, dict[str, Any]] = {}
        self.calls: list[tuple[str, dict[str, Any], str]] = []  # (tool, args, actor) for assertions
        self.playtest_metrics: Callable[[int], dict[str, Any]] = lambda n: {
            "completion_rate": min(1.0, 0.4 + 0.3 * n),
            "deaths": max(0, 3 - n),
        }
        self._hosts: dict[str, _Host] = {}
        self._pending: dict[int, asyncio.Future[dict[str, Any]]] = {}
        self._ids = itertools.count(1)
        self._playtests = 0
        self._register_builtin()
        for a in roster or [
            {"id": "nimbus", "name": "Nimbus", "role": "creative_director", "discipline": "direction"},
            {"id": "stratus", "name": "Stratus", "role": "gameplay_programmer", "discipline": "engineering"},
            {"id": "aurora", "name": "Aurora", "role": "environment_artist", "discipline": "art"},
            {"id": "critic", "name": "Critic", "role": "critic", "discipline": "qa"},
        ]:
            self.agents[a["id"]] = {"autonomy": "autonomous", "model": "", "provider": "anthropic", "memory": [], **a}

    # ------------------------------------------------------------------ plumbing
    def connect(self, client_name: str = "sky-agents") -> FakeConnection:
        return FakeConnection(self, client_name)

    def add_tool(
        self,
        name: str,
        handler: Handler,
        *,
        description: str = "",
        schema: dict[str, Any] | None = None,
        mutates: bool = False,
        category: str = "fake",
    ) -> None:
        self.tools[name] = FakeTool(name, description or name, schema or _obj(), handler, mutates, category)

    def emit(self, event: dict[str, Any]) -> None:
        self._seq += 1
        self.events.append({**event, "seq": self._seq, "time": time.time()})
        if len(self.events) > 4096:
            del self.events[: len(self.events) - 4096]
        cond = self._cond()
        if cond is not None:
            self._spawn(self._notify(cond))

    def _spawn(self, coro: Any) -> None:
        task = asyncio.ensure_future(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    async def _notify(self, cond: asyncio.Condition) -> None:
        async with cond:
            cond.notify_all()

    def _cond(self) -> asyncio.Condition | None:
        """The change notifier of the running event loop (None outside one)."""
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            return None
        if self._changed is None or self._cond_loop is not loop:
            self._changed = asyncio.Condition()
            self._cond_loop = loop
        return self._changed

    async def call(self, tool: str, args: dict[str, Any], actor: str) -> ToolResult:
        t = self.tools.get(tool)
        if t is None:
            close = _closest(tool, list(self.tools))
            return ToolResult.error(
                "unknown_tool",
                f"no tool named '{tool}'",
                f"did you mean '{close}'?" if close else "call tools/list",
                tool=tool,
            )
        for req in t.schema.get("required", []):
            if req not in args:
                return ToolResult.error("invalid_arguments", f"args.{req} is required", tool=tool)
        self.calls.append((tool, dict(args), actor))
        out = t.handler(dict(args), actor)
        result = await out if asyncio.iscoroutine(out) or isinstance(out, asyncio.Future) else out
        assert isinstance(result, ToolResult)
        result.tool = tool
        if not tool.startswith(("events_", "tool_host_")):
            self.emit(
                {
                    "type": "tool",
                    "actor": actor,
                    "tool": tool,
                    "ok": not result.is_error,
                    "mutates": t.mutates,
                    "summary": result.text[:160],
                }
            )
        return result

    def list_tools(self) -> list[dict[str, Any]]:
        return [t.spec() for t in self.tools.values()]

    # ------------------------------------------------------------------ helpers
    def member(self, actor: str, args: dict[str, Any]) -> str:
        """The roster member behind an actor ('mcp:sky-agents/mira' -> 'mira'), or `as`."""
        tail = actor.split("/", 1)[1] if "/" in actor else ""
        if tail in self.agents:
            return tail
        as_ = str(args.get("as") or "")
        if as_ in self.agents:
            return as_
        if actor.startswith("agent:") and actor[6:] in self.agents:
            return actor[6:]
        return ""

    def who(self, actor: str, args: dict[str, Any]) -> str:
        return self.member(actor, args) or actor

    def _studio(self, kind: str, action: str, id_: str, actor: str, summary: str, **extra: Any) -> None:
        self.emit(
            {"type": "studio", "kind": kind, "action": action, "id": id_, "actor": actor, "summary": summary, **extra}
        )

    def _next(self, prefix: str, items: list[dict[str, Any]]) -> str:
        return f"{prefix}-{len(items) + 1}"

    # ------------------------------------------------------------------ builtin tools
    def _register_builtin(self) -> None:
        add = self.add_tool

        def scene_overview(a: dict[str, Any], actor: str) -> ToolResult:
            lines = [f"#{e['id']} {e['name']} at {e.get('position', [0, 0, 0])}" for e in self.entities]
            return ToolResult.ok("\n".join(lines) or "(empty scene)", {"entities": self.entities})

        def entity_create(a: dict[str, Any], actor: str) -> ToolResult:
            e = {
                "id": len(self.entities) + 1,
                "name": a.get("name", "Entity"),
                "position": a.get("position", [0, 0, 0]),
                "tags": a.get("tags", []),
            }
            self.entities.append(e)
            return ToolResult.ok(f"created #{e['id']} {e['name']}", e)

        def entity_update(a: dict[str, Any], actor: str) -> ToolResult:
            for e in self.entities:
                if a.get("entity") in (e["id"], e["name"]):
                    e.update({k: v for k, v in a.items() if k != "entity"})
                    return ToolResult.ok(f"updated #{e['id']}", e)
            return ToolResult.error("not_found", f"no entity {a.get('entity')}", "scene_overview lists entities")

        def viewport_capture(a: dict[str, Any], actor: str) -> ToolResult:
            return ToolResult.ok(
                "captured 1x1", {"width": 1, "height": 1, "visible": len(self.entities)}, images=[TINY_PNG]
            )

        def playtest_run(a: dict[str, Any], actor: str) -> ToolResult:
            self._playtests += 1
            pid = f"P-{self._playtests}"
            metrics = self.playtest_metrics(self._playtests - 1)
            findings = (
                []
                if metrics.get("deaths", 0) == 0
                else [
                    {
                        "category": "difficulty",
                        "severity": "high",
                        "summary": "Players die at the lava bridge",
                        "fingerprint": "deaths:lava",
                    }
                ]
            )
            self._studio("playtest", "finished", pid, actor, f"{pid}: {metrics}")
            return ToolResult.ok(f"{pid} {metrics}", {"id": pid, "metrics": metrics, "findings": findings})

        add("scene_overview", scene_overview, description="The scene outline", category="scene")
        add(
            "entity_create",
            entity_create,
            schema=_obj({"name": S, "position": A, "mesh": S, "tags": A}),
            mutates=True,
            category="entity",
            description="Create an entity",
        )
        add(
            "entity_update",
            entity_update,
            schema=_obj({"entity": {}}, ["entity"]),
            mutates=True,
            category="entity",
            description="Update an entity",
        )
        add(
            "viewport_capture",
            viewport_capture,
            schema=_obj({"width": I, "height": I, "annotate": B}),
            category="view",
            description="Capture the viewport",
        )
        add(
            "playtest_run",
            playtest_run,
            schema=_obj({"policy": S, "runs": I, "seconds": {"type": "number"}}),
            mutates=True,
            category="studio",
            description="Play the game with a bot",
        )

        # --- roster
        def agent_list(a: dict[str, Any], actor: str) -> ToolResult:
            agents = [{**p, "status": self.presence.get(p["id"], {"status": "idle"})} for p in self.agents.values()]
            return ToolResult.ok("\n".join(f"@{p['id']} {p['role']}" for p in agents), {"agents": agents})

        def agent_define(a: dict[str, Any], actor: str) -> ToolResult:
            aid = str(a.get("id") or re.sub(r"[^a-z0-9]+", "-", str(a.get("name", "agent")).lower()).strip("-"))
            p = self.agents.setdefault(
                aid,
                {
                    "id": aid,
                    "name": a.get("name", aid),
                    "autonomy": "autonomous",
                    "memory": [],
                    "provider": "anthropic",
                    "model": "",
                },
            )
            p.update({k: v for k, v in a.items() if k != "as"})
            p["id"] = aid
            self._studio("agent", "defined", aid, actor, f"@{aid}")
            return ToolResult.ok(f"defined @{aid}", p)

        def agent_brief(a: dict[str, Any], actor: str) -> ToolResult:
            p = self.agents.get(str(a.get("agent")))
            if p is None:
                return ToolResult.error("not_found", f"no agent '{a.get('agent')}'", "studio_agent_list shows ids")
            tools = []
            for t in self.tools.values():
                if t.name.startswith(("events_", "tool_host_")):
                    continue
                access = "allow"
                if t.mutates and t.category not in ("studio",):
                    access = {"observe": "off", "ask": "ask"}.get(p.get("autonomy", "autonomous"), "allow")
                if access != "off":
                    tools.append({"name": t.name, "category": t.category, "access": access})
            prompt = (
                f"You are {p.get('name')} (@{p['id']}), the studio's {p.get('role')}. "
                f"{p.get('mission', '')} {p.get('instructions', '')}".strip()
            )
            if p.get("memory"):
                prompt += "\nMemory:\n" + "\n".join(f"- {m}" for m in p["memory"])
            return ToolResult.ok(
                prompt,
                {
                    "agent": p["id"],
                    "actor": f"agent:{p['id']}",
                    "model": p.get("model", ""),
                    "provider": p.get("provider", "anthropic"),
                    "max_rounds": 40,
                    "system_prompt": prompt,
                    "tools": tools,
                },
            )

        add(
            "studio_agent_list",
            agent_list,
            schema=_obj({"include_profiles": B}),
            category="studio",
            description="The roster",
        )
        add(
            "studio_agent_define",
            agent_define,
            schema=_obj({"name": S, "id": S, "role": S}),
            mutates=True,
            category="studio",
            description="Define an agent",
        )
        add(
            "studio_agent_brief",
            agent_brief,
            schema=_obj({"agent": S, "loop_member": B}, ["agent"]),
            category="studio",
            description="An agent's system prompt and tools",
        )

        def presence(a: dict[str, Any], actor: str) -> ToolResult:
            who = str(a.get("agent") or self.member(actor, a))
            if not who:
                return ToolResult.error(
                    "invalid_arguments", "who? this call needs a roster identity", "pass agent or as"
                )
            self.presence[who] = {"status": a["status"], "activity": a.get("activity", "")}
            self._studio(
                "agent",
                "presence",
                who,
                f"agent:{who}",
                a["status"],
                status=a["status"],
                activity=a.get("activity", ""),
            )
            return ToolResult.ok(f"@{who} is {a['status']}", {"agent": who, "presence": self.presence[who]})

        def usage_report(a: dict[str, Any], actor: str) -> ToolResult:
            who = str(a.get("agent") or self.member(actor, a))
            if not who:
                return ToolResult.error("invalid_arguments", "who? this call needs a roster identity", "pass as")
            u = self.usage.setdefault(
                who,
                {"input_tokens": 0, "output_tokens": 0, "cache_read_tokens": 0, "cache_write_tokens": 0, "requests": 0},
            )
            for k in ("input_tokens", "output_tokens", "cache_read_tokens", "cache_write_tokens"):
                u[k] += int(a.get(k, 0))
            u["requests"] += 1
            return ToolResult.ok(f"recorded usage for @{who}", dict(u))

        def memory(a: dict[str, Any], actor: str) -> ToolResult:
            who = str(a.get("agent") or self.member(actor, a))
            if who not in self.agents:
                return ToolResult.error("invalid_arguments", "who? this call needs a roster identity", "pass as")
            notes: list[str] = self.agents[who].setdefault("memory", [])
            if a["action"] == "note":
                notes.append(str(a.get("text", "")))
            elif a["action"] == "forget":
                n = int(a.get("number", 0))
                if 1 <= n <= len(notes):
                    notes.pop(n - 1)
            return ToolResult.ok(
                "\n".join(f"{i + 1}. {n}" for i, n in enumerate(notes)) or "(no notes)",
                {"agent": who, "memory": list(notes)},
            )

        add(
            "studio_presence",
            presence,
            schema=_obj({"status": S, "activity": S, "agent": S, "as": S}, ["status"]),
            mutates=True,
            category="studio",
            description="Set presence",
        )
        add(
            "studio_usage_report",
            usage_report,
            schema=_obj(
                {
                    "agent": S,
                    "model": S,
                    "input_tokens": I,
                    "output_tokens": I,
                    "cache_read_tokens": I,
                    "cache_write_tokens": I,
                    "as": S,
                }
            ),
            mutates=True,
            category="studio",
            description="Report usage",
        )
        add(
            "studio_memory",
            memory,
            schema=_obj({"action": S, "text": S, "number": I, "agent": S, "as": S}, ["action"]),
            mutates=True,
            category="studio",
            description="Agent memory notes",
        )

        # --- messages
        def send(a: dict[str, Any], actor: str) -> ToolResult:
            text = str(a.get("text", "")).strip()
            if not text:
                return ToolResult.error("invalid_arguments", "a message needs text")
            frm = self.who(actor, a)
            m: dict[str, Any] = {
                "id": self._next("M", self.messages),
                "at": _now(),
                "from": frm,
                "channel": str(a.get("channel", "general")).lstrip("#") or "general",
                "text": text,
            }
            to = [t.lstrip("@") for t in a.get("to", [])]
            for t in to:
                if t not in self.agents:
                    return ToolResult.error("not_found", f"no agent '{t}'", "studio_agent_list shows ids")
            mentions = [x for x in re.findall(r"(?<![\w])@([a-z0-9_-]+)", text) if x in self.agents]
            if a.get("reply_to"):
                parent = next((x for x in self.messages if x["id"] == a["reply_to"]), None)
                if parent is None:
                    return ToolResult.error("not_found", f"no message {a['reply_to']}")
                m["thread"] = parent.get("thread") or parent["id"]
                if "channel" not in a:
                    m["channel"] = parent["channel"]
                if parent["from"] not in to:
                    to.append(parent["from"])
            to = [t for t in to if t != frm]
            if to:
                m["to"] = to
            if mentions:
                m["mentions"] = mentions
            kind = str(a.get("kind", "chat") or "chat")
            if kind != "chat":
                m["kind"] = kind
            if a.get("data"):
                m["data"] = a["data"]
            refs = {k: a[k] for k in ("task", "feedback") if k in a}
            if refs:
                m["refs"] = refs
            self.messages.append(m)
            self._studio(
                "message",
                "sent",
                m["id"],
                actor,
                f"{frm}: {text[:100]}",
                channel=m["channel"],
                to=m.get("to", []),
                mentions=mentions,
                message=m,
            )
            return ToolResult.ok(f"sent {m['id']} in #{m['channel']}", m)

        def inbox(a: dict[str, Any], actor: str) -> ToolResult:
            after = _num(a.get("after", ""))
            limit = int(a.get("limit", 30))
            if "thread" in a:
                msgs = [
                    m
                    for m in self.messages
                    if (m["id"] == a["thread"] or m.get("thread") == a["thread"]) and _num(m["id"]) > after
                ]
                return ToolResult.ok(_lines(msgs[-limit:]), {"thread": a["thread"], "messages": msgs[-limit:]})
            if "channel" in a:
                ch = str(a["channel"]).lstrip("#")
                msgs = [m for m in self.messages if (ch == "*" or m["channel"] == ch) and _num(m["id"]) > after]
                return ToolResult.ok(_lines(msgs[-limit:]), {"channel": ch, "messages": msgs[-limit:]})
            who = str(a.get("agent") or self.member(actor, a))
            msgs = [
                m
                for m in self.messages
                if m["from"] != who
                and (
                    who in m.get("to", [])
                    or who in m.get("mentions", [])
                    or (not m.get("to") and not m.get("mentions") and m["channel"] == "general")
                )
            ]
            return ToolResult.ok(_lines(msgs[-limit:]), {"agent": who, "messages": msgs[-limit:]})

        add(
            "studio_message_send",
            send,
            schema=_obj(
                {
                    "text": S,
                    "to": A,
                    "channel": S,
                    "reply_to": S,
                    "task": S,
                    "feedback": S,
                    "kind": S,
                    "data": O,
                    "as": S,
                },
                ["text"],
            ),
            mutates=True,
            category="studio",
            description="Send message",
        )
        add(
            "studio_inbox",
            inbox,
            schema=_obj({"agent": S, "unread_only": B, "channel": S, "thread": S, "after": S, "limit": I, "as": S}),
            category="studio",
            description="Inbox",
        )

        # --- board
        def task_create(a: dict[str, Any], actor: str) -> ToolResult:
            t = {
                "id": self._next("T", self.tasks),
                "title": a["title"],
                "description": a.get("description", ""),
                "acceptance": a.get("acceptance", []),
                "discipline": a.get("discipline", ""),
                "assignee": str(a.get("assignee", "")).lstrip("@"),
                "status": a.get("status", "todo"),
                "priority": a.get("priority", "normal"),
                "comments": [],
                "created_by": self.who(actor, a),
                "links": {"feedback": a.get("feedback", [])},
            }
            self.tasks.append(t)
            self._studio("task", "created", t["id"], actor, t["title"], task=t)
            return ToolResult.ok(f"created {t['id']}: {t['title']}", t)

        def task_update(a: dict[str, Any], actor: str) -> ToolResult:
            t = next((x for x in self.tasks if x["id"] == a["task"]), None)
            if t is None:
                return ToolResult.error("not_found", f"no task {a['task']}", "studio_task_list shows tasks")
            for k in ("status", "assignee", "title", "description", "priority"):
                if k in a:
                    t[k] = a[k]
            if a.get("comment"):
                t["comments"].append({"by": self.who(actor, a), "text": a["comment"]})
            self._studio("task", "updated", t["id"], actor, f"{t['id']} {t['status']}", task=t)
            if t["status"] == "done":
                for fid in t["links"].get("feedback", []):
                    for f in self.feedback:
                        if f["id"] == fid and all(
                            x["status"] == "done" for x in self.tasks if fid in x["links"].get("feedback", [])
                        ):
                            f["status"] = "fixed"
            return ToolResult.ok(f"{t['id']} → {t['status']}", t)

        def task_list(a: dict[str, Any], actor: str) -> ToolResult:
            ts = self.tasks
            st = a.get("status")
            if st == "open":
                ts = [t for t in ts if t["status"] in ("backlog", "todo", "doing", "review")]
            elif st:
                ts = [t for t in ts if t["status"] == st]
            if a.get("assignee"):
                ts = [t for t in ts if t["assignee"] == str(a["assignee"]).lstrip("@")]
            if a.get("mine"):
                me = self.member(actor, a)
                ts = [t for t in ts if t["assignee"] == me]
            return ToolResult.ok(
                "\n".join(f"{t['id']} [{t['status']}] {t['title']} @{t['assignee']}" for t in ts) or "(no tasks)",
                {"tasks": ts},
            )

        def task_claim(a: dict[str, Any], actor: str) -> ToolResult:
            me = self.member(actor, a)
            if not me:
                return ToolResult.error("invalid_arguments", "who? this call needs a roster identity", "pass as")
            if a.get("task"):
                t = next((x for x in self.tasks if x["id"] == a["task"]), None)
            else:
                t = next((x for x in self.tasks if x["status"] == "todo" and x["assignee"] in ("", me)), None)
            if t is None:
                return ToolResult.error("not_found", "no task to claim", "studio_task_list shows the board")
            t["assignee"], t["status"] = me, "doing"
            self.presence[me] = {"status": "working", "activity": f"{t['id']} {t['title']}"}
            self._studio("task", "claimed", t["id"], actor, f"@{me} took {t['id']}", task=t)
            return ToolResult.ok(f"@{me} took {t['id']}", t)

        add(
            "studio_task_create",
            task_create,
            schema=_obj(
                {
                    "title": S,
                    "description": S,
                    "acceptance": A,
                    "discipline": S,
                    "assignee": S,
                    "priority": S,
                    "status": S,
                    "feedback": A,
                    "as": S,
                },
                ["title"],
            ),
            mutates=True,
            category="studio",
            description="Create task",
        )
        add(
            "studio_task_update",
            task_update,
            schema=_obj(
                {
                    "task": S,
                    "status": S,
                    "assignee": S,
                    "comment": S,
                    "title": S,
                    "description": S,
                    "priority": S,
                    "as": S,
                },
                ["task"],
            ),
            mutates=True,
            category="studio",
            description="Update task",
        )
        add(
            "studio_task_list",
            task_list,
            schema=_obj({"status": S, "assignee": S, "mine": B, "limit": I, "as": S}),
            category="studio",
            description="Board",
        )
        add(
            "studio_task_claim",
            task_claim,
            schema=_obj({"task": S, "as": S}),
            mutates=True,
            category="studio",
            description="Claim task",
        )

        # --- feedback & decisions
        def fb_submit(a: dict[str, Any], actor: str) -> ToolResult:
            fp = a.get("fingerprint")
            for f in self.feedback:
                if fp and f.get("fingerprint") == fp and f["status"] not in ("verified", "dropped"):
                    f["occurrences"] += 1
                    return ToolResult.ok(f"{f['id']} occurred again", {**f, "merged": True})
            f = {
                "id": self._next("F", self.feedback),
                "by": self.who(actor, a),
                "category": a.get("category", "bug"),
                "severity": a.get("severity", "medium"),
                "summary": a["summary"],
                "details": a.get("details", ""),
                "evidence": a.get("evidence", {}),
                "status": "open",
                "fingerprint": fp or "",
                "occurrences": 1,
            }
            self.feedback.append(f)
            self._studio("feedback", "submitted", f["id"], actor, f["summary"], feedback=f)
            return ToolResult.ok(f"filed {f['id']}: {f['summary']}", f)

        def fb_list(a: dict[str, Any], actor: str) -> ToolResult:
            st = a.get("status", "all")
            fs = self.feedback
            if st == "open":
                fs = [f for f in fs if f["status"] == "open"]
            elif st == "needs_decision":
                fs = [f for f in fs if f["status"] in ("open", "regressed")]
            elif st not in ("all", None):
                fs = [f for f in fs if f["status"] == st]
            return ToolResult.ok(
                "\n".join(f"{f['id']} [{f['status']}] {f['summary']}" for f in fs) or "(none)", {"feedback": fs}
            )

        def decide(a: dict[str, Any], actor: str) -> ToolResult:
            f = next((x for x in self.feedback if x["id"] == a["feedback"]), None)
            if f is None:
                return ToolResult.error("not_found", f"no feedback {a['feedback']}")
            me = self.member(actor, a)
            if me and self.agents[me].get("discipline") not in ("direction", "production"):
                return ToolResult.error("permission_denied", f"@{me} cannot decide", "only directors/producers decide")
            d = {
                "id": self._next("D", self.decisions),
                "feedback": f["id"],
                "verdict": a["verdict"],
                "rationale": a.get("rationale", ""),
                "by": self.who(actor, a),
                "tasks": [],
            }
            if a["verdict"] == "act":
                f["status"] = "accepted"
                for spec in a.get("tasks", []):
                    r = task_create({**spec, "feedback": [f["id"]]}, actor)
                    d["tasks"].append(r.data["id"])
            else:
                f["status"] = {"drop": "dropped", "defer": "deferred"}.get(a["verdict"], "dropped")
            f["decision"] = d["id"]
            self.decisions.append(d)
            self._studio("decision", "made", d["id"], actor, f"{f['id']}: {d['verdict']}", decision=d)
            return ToolResult.ok(f"{d['id']}: {a['verdict']} {f['id']}", d)

        add(
            "studio_feedback_submit",
            fb_submit,
            schema=_obj(
                {
                    "category": S,
                    "severity": S,
                    "summary": S,
                    "details": S,
                    "evidence": O,
                    "fingerprint": S,
                    "target": S,
                    "as": S,
                },
                ["summary"],
            ),
            mutates=True,
            category="studio",
            description="File feedback",
        )
        add(
            "studio_feedback_list",
            fb_list,
            schema=_obj({"status": S, "limit": I}),
            category="studio",
            description="Feedback",
        )
        add(
            "studio_decide",
            decide,
            schema=_obj({"feedback": S, "verdict": S, "rationale": S, "tasks": A, "as": S}, ["feedback", "verdict"]),
            mutates=True,
            category="studio",
            description="Decide on feedback",
        )

        # --- loops (simplified: agent stages only, max_iterations)
        def loop_define(a: dict[str, Any], actor: str) -> ToolResult:
            name = str(a.get("name") or a.get("template") or "loop")
            stages = a.get("stages") or [
                {"id": "build", "assignees": ["aurora"], "instruction": "Work on: {{goal}}"},
                {"id": "review", "assignees": ["critic"], "instruction": "Review: {{inputs}}"},
            ]
            self.loops[name] = {
                "name": name,
                "goal": a.get("goal", ""),
                "stages": stages,
                "stop": {"max_iterations": int((a.get("stop") or {}).get("max_iterations", 2))},
                "state": {"status": "idle"},
            }
            return ToolResult.ok(f"defined loop {name}", self.loops[name])

        def assignments(loop: dict[str, Any]) -> list[dict[str, Any]]:
            st = loop["state"]
            stage = loop["stages"][st["stage_index"]]
            reports = "\n".join(f"@{r['agent']}: {r['report']}" for r in st["reports"])
            out = []
            for who in stage["assignees"]:
                prompt = stage["instruction"].replace("{{goal}}", loop["goal"]).replace("{{inputs}}", reports)
                out.append({"agent": who.lstrip("@"), "stage": stage["id"], "prompt": prompt, "tasks": []})
            return out

        def loop_status_payload(loop: dict[str, Any]) -> dict[str, Any]:
            st = loop["state"]
            return {
                "loop": loop["name"],
                "status": st["status"],
                "iteration": st.get("iteration", 0),
                "stage": st.get("stage", ""),
                "assignments": st.get("pending", []),
                "parallel": True,
            }

        def loop_start(a: dict[str, Any], actor: str) -> ToolResult:
            loop = self.loops.get(a["loop"])
            if loop is None:
                return ToolResult.error("not_found", f"no loop {a['loop']}", "studio_loop_define first")
            if a.get("goal"):
                loop["goal"] = a["goal"]
            if a.get("max_iterations"):
                loop["stop"]["max_iterations"] = int(a["max_iterations"])
            loop["state"] = {"status": "running", "iteration": 1, "stage_index": 0, "reports": []}
            loop["state"]["stage"] = loop["stages"][0]["id"]
            loop["state"]["pending"] = assignments(loop)
            self._studio("loop", "started", loop["name"], actor, loop["name"])
            return ToolResult.ok(f"loop {loop['name']} running", loop_status_payload(loop))

        def loop_advance(a: dict[str, Any], actor: str) -> ToolResult:
            loop = self.loops.get(a["loop"])
            if loop is None:
                return ToolResult.error("not_found", f"no loop {a['loop']}")
            st = loop["state"]
            for r in a.get("reports", []):
                st["reports"].append({"agent": r["agent"], "report": r.get("report", "")})
                st["pending"] = [p for p in st["pending"] if p["agent"] != r["agent"]]
                if "SIGNOFF" in str(r.get("report", "")):
                    st["signoff"] = True
            if st["pending"]:
                return ToolResult.ok("waiting for reports", loop_status_payload(loop))
            st["stage_index"] += 1
            if st["stage_index"] >= len(loop["stages"]):
                if st.get("signoff") or st["iteration"] >= loop["stop"]["max_iterations"]:
                    st["status"] = "done"
                    st["pending"] = []
                    self._studio("loop", "finished", loop["name"], actor, loop["name"])
                    return ToolResult.ok("loop done", loop_status_payload(loop))
                st["iteration"] += 1
                st["stage_index"] = 0
                st["reports"] = []
            st["stage"] = loop["stages"][st["stage_index"]]["id"]
            st["pending"] = assignments(loop)
            return ToolResult.ok(f"stage {st['stage']}", loop_status_payload(loop))

        def loop_status(a: dict[str, Any], actor: str) -> ToolResult:
            if a.get("loop"):
                loop = self.loops.get(a["loop"])
                if loop is None:
                    return ToolResult.error("not_found", f"no loop {a['loop']}")
                return ToolResult.ok(loop["state"]["status"], loop_status_payload(loop))
            return ToolResult.ok("loops", {"loops": [loop_status_payload(x) for x in self.loops.values()]})

        def loop_stop(a: dict[str, Any], actor: str) -> ToolResult:
            loop = self.loops.get(a["loop"])
            if loop is None:
                return ToolResult.error("not_found", f"no loop {a['loop']}")
            loop["state"]["status"] = "stopped"
            loop["state"]["pending"] = []
            return ToolResult.ok("stopped", loop_status_payload(loop))

        add(
            "studio_loop_define",
            loop_define,
            schema=_obj({"name": S, "template": S, "goal": S, "stages": A, "stop": O}),
            mutates=True,
            category="studio",
            description="Define a loop",
        )
        add(
            "studio_loop_start",
            loop_start,
            schema=_obj({"loop": S, "goal": S, "max_iterations": I}, ["loop"]),
            mutates=True,
            category="studio",
            description="Start a loop",
        )
        add(
            "studio_loop_advance",
            loop_advance,
            schema=_obj({"loop": S, "reports": A, "approve": B, "complete_stage": B}, ["loop"]),
            mutates=True,
            category="studio",
            description="Advance a loop",
        )
        add("studio_loop_status", loop_status, schema=_obj({"loop": S}), category="studio", description="Loop status")
        add(
            "studio_loop_stop",
            loop_stop,
            schema=_obj({"loop": S, "reason": S}, ["loop"]),
            mutates=True,
            category="studio",
            description="Stop a loop",
        )

        # --- agent link: events and tool hosting
        async def events_poll(a: dict[str, Any], actor: str) -> ToolResult:
            since = int(a.get("since", 0))
            if since > self._seq:
                since = 0
            limit = int(a.get("limit", 200))
            wait = min(int(a.get("wait_ms", 0)), 25000) / 1000.0
            types, actors, excl = a.get("types") or [], a.get("actors") or [], a.get("exclude_actors") or []

            def match(e: dict[str, Any]) -> bool:
                t = e.get("type", "")
                if types and not any(x in (t, f"{t}.{e.get('kind', '')}") for x in types):
                    return False
                if actors and not any(str(e.get("actor", "")).startswith(x) for x in actors):
                    return False
                return not (excl and any(str(e.get("actor", "")).startswith(x) for x in excl))

            def collect() -> tuple[list[dict[str, Any]], int]:
                out: list[dict[str, Any]] = []
                nxt = since
                for e in self.events:
                    if e["seq"] <= since:
                        continue
                    if len(out) >= limit:
                        break
                    nxt = e["seq"]
                    if match(e):
                        out.append(e)
                return out, nxt

            found, nxt = collect()
            deadline = time.monotonic() + wait
            cond = self._cond()
            while not found and cond is not None and time.monotonic() < deadline:
                async with cond:
                    try:
                        await asyncio.wait_for(cond.wait(), max(0.0, deadline - time.monotonic()))
                    except asyncio.TimeoutError:
                        break
                since = nxt
                found, nxt = collect()
            return ToolResult.ok(
                f"{len(found)} event(s), next={nxt}",
                {
                    "events": found,
                    "next": nxt,
                    "last_seq": self._seq,
                    "truncated": False,
                    "more": False,
                    "reset": False,
                },
            )

        def host_register(a: dict[str, Any], actor: str) -> ToolResult:
            hid = str(a.get("host") or f"H-{len(self._hosts) + 1}")
            names = []
            for spec in a["tools"]:
                name = spec["name"] if spec["name"].startswith("py_") else "py_" + spec["name"]
                if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", name):
                    return ToolResult.error("invalid_arguments", f"invalid tool name '{name}'")
                names.append(name)

                def make(tool_name: str) -> Handler:
                    async def forward(args: dict[str, Any], caller: str) -> ToolResult:
                        host = self._hosts.get(hid)
                        if host is None or tool_name not in host.tools:
                            return ToolResult.error("host_gone", f"nothing serves {tool_name} any more")
                        cid = next(self._ids)
                        fut: asyncio.Future[dict[str, Any]] = asyncio.get_running_loop().create_future()
                        self._pending[cid] = fut
                        host.queue.append({"call": cid, "tool": tool_name, "args": args, "actor": caller})
                        cond = self._cond()
                        if cond is not None:
                            await self._notify(cond)
                        try:
                            payload = await asyncio.wait_for(fut, 120)
                        finally:
                            self._pending.pop(cid, None)
                        return ToolResult.from_mcp(tool_name, payload)

                    return forward

                self.add_tool(
                    name,
                    make(name),
                    description=spec.get("description", ""),
                    schema=spec.get("input_schema") or _obj(),
                    mutates=bool(spec.get("mutates")),
                    category="python",
                )
            old = self._hosts.get(hid)
            if old:
                for n in old.tools:
                    if n not in names:
                        self.tools.pop(n, None)
            self._hosts[hid] = _Host(hid, names, old.queue if old else [])
            self.emit({"type": "tool_host", "action": "register", "host": hid, "actor": actor, "tools": names})
            return ToolResult.ok(f"serving {len(names)} tool(s) as host {hid}", {"host": hid, "tools": names})

        async def host_poll(a: dict[str, Any], actor: str) -> ToolResult:
            host = self._hosts.get(a["host"])
            if host is None:
                return ToolResult.error("not_found", f"no tool host {a['host']}", "register again")
            deadline = time.monotonic() + min(int(a.get("wait_ms", 20000)), 25000) / 1000.0
            cond = self._cond()
            while not host.queue and cond is not None and time.monotonic() < deadline:
                async with cond:
                    try:
                        await asyncio.wait_for(cond.wait(), max(0.0, deadline - time.monotonic()))
                    except asyncio.TimeoutError:
                        break
                if a["host"] not in self._hosts:
                    break
            calls, host.queue = host.queue[: int(a.get("max", 8))], host.queue[int(a.get("max", 8)) :]
            return ToolResult.ok(f"{len(calls)} call(s)", {"calls": calls})

        def host_reply(a: dict[str, Any], actor: str) -> ToolResult:
            fut = self._pending.get(int(a["call"]))
            if fut is None or fut.done():
                return ToolResult.error("not_found", f"call {a['call']} is not waiting any more")
            content = [{"type": "text", "text": a.get("text") or ""}]
            content += [{"type": "image", "data": i, "mimeType": "image/png"} for i in a.get("images", [])]
            payload: dict[str, Any] = {"content": content, "isError": bool(a.get("is_error"))}
            if a.get("structured") is not None:
                payload["structuredContent"] = a["structured"]
            fut.set_result(payload)
            return ToolResult.ok("delivered")

        def host_unregister(a: dict[str, Any], actor: str) -> ToolResult:
            host = self._hosts.pop(a["host"], None)
            if host is None:
                return ToolResult.error("not_found", f"no tool host {a['host']}")
            for n in host.tools:
                self.tools.pop(n, None)
            cond = self._cond()
            if cond is not None:
                self._spawn(self._notify(cond))
            return ToolResult.ok(f"host {a['host']} stopped serving tools")

        def host_list(a: dict[str, Any], actor: str) -> ToolResult:
            hosts = [{"host": h.id, "tools": h.tools, "queued": len(h.queue)} for h in self._hosts.values()]
            return ToolResult.ok(f"{len(hosts)} host(s)", {"hosts": hosts, "events": {"last_seq": self._seq}})

        add(
            "events_poll",
            events_poll,
            schema=_obj({"since": I, "types": A, "actors": A, "exclude_actors": A, "limit": I, "wait_ms": I}),
            category="agent",
            description="Follow engine events",
        )
        add(
            "tool_host_register",
            host_register,
            schema=_obj({"tools": A, "host": S, "label": S, "ttl_seconds": I, "replace": B}, ["tools"]),
            category="agent",
            description="Serve tools",
            mutates=True,
        )
        add(
            "tool_host_poll",
            host_poll,
            schema=_obj({"host": S, "wait_ms": I, "max": I}, ["host"]),
            category="agent",
            description="Fetch calls",
        )
        add(
            "tool_host_reply",
            host_reply,
            schema=_obj(
                {"host": S, "call": I, "text": S, "structured": O, "images": A, "is_error": B}, ["host", "call"]
            ),
            category="agent",
            description="Answer a call",
        )
        add(
            "tool_host_unregister",
            host_unregister,
            schema=_obj({"host": S}, ["host"]),
            category="agent",
            description="Stop serving",
            mutates=True,
        )
        add("tool_host_list", host_list, category="agent", description="Tool hosts")


class FakeConnection:
    """One attributed session on a :class:`FakeEngine` (actor ``mcp:<client name>``)."""

    def __init__(self, engine: FakeEngine, client_name: str) -> None:
        self.engine = engine
        self.client_name = client_name
        self._closed = False

    @property
    def closed(self) -> bool:
        return self._closed

    @property
    def actor(self) -> str:
        return f"mcp:{self.client_name}"

    async def call_tool(
        self, name: str, arguments: dict[str, Any] | None = None, timeout: float | None = None
    ) -> ToolResult:
        coro = self.engine.call(name, dict(arguments or {}), self.actor)
        return await (asyncio.wait_for(coro, timeout) if timeout else coro)

    async def request(self, method: str, params: dict[str, Any] | None = None, timeout: float | None = None) -> Any:
        if method == "tools/list":
            return {"tools": self.engine.list_tools()}
        if method == "tools/call":
            p = params or {}
            return (await self.call_tool(str(p["name"]), p.get("arguments"))).to_mcp()
        if method == "ping":
            return {}
        raise NotImplementedError(method)

    async def close(self) -> None:
        self._closed = True


def _has_loop() -> bool:
    try:
        asyncio.get_running_loop()
        return True
    except RuntimeError:
        return False


def _now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _num(mid: str) -> int:
    m = re.search(r"(\d+)$", str(mid))
    return int(m.group(1)) if m else 0


def _lines(msgs: list[dict[str, Any]]) -> str:
    return "\n".join(f"{m['id']} #{m['channel']} @{m['from']}: {m['text']}" for m in msgs) or "(no messages)"


def _closest(name: str, names: list[str]) -> str:
    import difflib

    got = difflib.get_close_matches(name, names, n=1)
    return got[0] if got else ""
