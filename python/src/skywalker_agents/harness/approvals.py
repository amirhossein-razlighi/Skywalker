"""Human-in-the-loop approvals and the policy that decides when one is needed.

* :class:`ApprovalPolicy` decides which tool calls need a human: tools a role's brief marks ``ask``,
  mutating tools for ``ask``-autonomy agents, explicit patterns, or a tool's own flag.
* An :class:`Approver` gets the answer: automatically, from a callback, from the terminal, or from the
  editor (:class:`StudioApprover` posts in the Studio panel's ``#approvals`` channel; the human answers
  ``approve A-1f2e`` / ``deny A-1f2e too risky`` there).
"""

from __future__ import annotations

import asyncio
import fnmatch
import json
import re
import secrets
import sys
import time
from collections.abc import Awaitable, Callable, Sequence
from typing import TYPE_CHECKING, Any, Literal, Protocol

from pydantic import BaseModel, Field

if TYPE_CHECKING:  # pragma: no cover
    from ..engine.client import AsyncEngine
    from ..tools.base import Tool


class ApprovalRequest(BaseModel):
    id: str = Field(default_factory=lambda: "A-" + secrets.token_hex(3))
    agent: str = ""
    tool: str
    args: dict[str, Any] = Field(default_factory=dict)
    reason: str = ""

    def summary(self, n: int = 300) -> str:
        args = json.dumps(self.args, default=str)
        if len(args) > n:
            args = args[:n] + "…"
        return f"@{self.agent or '?'} wants to run {self.tool} {args}" + (f" ({self.reason})" if self.reason else "")


class Decision(BaseModel):
    approved: bool
    by: str = ""
    note: str = ""


class Approver(Protocol):
    async def request(self, req: ApprovalRequest) -> Decision: ...


class AutoApprove:
    async def request(self, req: ApprovalRequest) -> Decision:
        return Decision(approved=True, by="auto")


class AutoDeny:
    def __init__(self, note: str = "no human is available to approve this") -> None:
        self.note = note

    async def request(self, req: ApprovalRequest) -> Decision:
        return Decision(approved=False, by="auto", note=self.note)


class CallbackApprover:
    """Delegates to a function (sync or async) returning bool or :class:`Decision`."""

    def __init__(self, fn: Callable[[ApprovalRequest], bool | Decision | Awaitable[bool | Decision]]) -> None:
        self.fn = fn

    async def request(self, req: ApprovalRequest) -> Decision:
        out = self.fn(req)
        if asyncio.iscoroutine(out) or isinstance(out, asyncio.Future):
            out = await out
        if isinstance(out, Decision):
            return out
        return Decision(approved=bool(out), by="callback")


class TerminalApprover:
    """Asks on the terminal (y/N). Denies when stdin is not interactive."""

    async def request(self, req: ApprovalRequest) -> Decision:
        if not sys.stdin.isatty():
            return Decision(approved=False, by="terminal", note="no interactive terminal")
        answer = await asyncio.to_thread(input, f"\n[approval {req.id}] {req.summary()}\nApprove? [y/N] ")
        ok = answer.strip().lower() in ("y", "yes")
        return Decision(approved=ok, by="terminal")


_ANSWER = re.compile(
    r"\b(approve[ds]?|yes|ok|allow|deny|denied|no|reject(?:ed)?)\b\s*(A-[0-9a-f]{6})?\s*(.*)", re.I | re.S
)


class StudioApprover:
    """Routes approvals to the editor: a message in ``#approvals`` that a human answers in the Studio panel.

    The answer is any human (non-agent) message in that channel saying approve/deny with the request id,
    or a reply in the request's thread. No answer within ``timeout`` seconds is a denial.
    """

    def __init__(
        self,
        engine: AsyncEngine,
        *,
        channel: str = "approvals",
        timeout: float = 600,
        human_actors: Sequence[str] = ("user",),
    ) -> None:
        self.engine = engine
        self.channel = channel
        self.timeout = timeout
        self.human_actors = tuple(human_actors)

    def _is_human(self, sender: str, roster: set[str]) -> bool:
        return sender in self.human_actors or (sender not in roster and not sender.startswith("mcp:sky-agents"))

    async def request(self, req: ApprovalRequest) -> Decision:
        roster_res = await self.engine.call("studio_agent_list", {})
        roster = {a.get("id") for a in roster_res.data.get("agents", [])}
        stream = self.engine.events(types=["studio.message"])
        try:
            await stream.poll(wait_ms=0)  # pin the cursor before asking
            sent = await self.engine.call(
                "studio_message_send",
                {
                    "channel": self.channel,
                    "kind": "approval_request",
                    "text": f"Approval {req.id}: {req.summary()}. Reply 'approve {req.id}' or 'deny {req.id} <why>' "
                    f"here.",
                    "data": req.model_dump(mode="json"),
                },
                check=True,
            )
            root = str(sent.data.get("id", ""))
            deadline = time.monotonic() + self.timeout
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return Decision(approved=False, by="timeout", note=f"no answer within {self.timeout:.0f} s")
                ev = await stream.next(timeout=remaining)
                if ev is None:
                    continue
                msg = ev.get("message") or {}
                sender = str(msg.get("from", ""))
                if msg.get("id") == root or not self._is_human(sender, roster):
                    continue
                in_thread = msg.get("thread") == root
                if not in_thread and msg.get("channel") != self.channel:
                    continue
                m = _ANSWER.search(str(msg.get("text", "")))
                if m is None or (m.group(2) and m.group(2) != req.id) or (not m.group(2) and not in_thread):
                    continue
                verdict = m.group(1).lower()
                approved = verdict.startswith(("approve", "yes", "ok", "allow"))
                note = m.group(3).strip()
                await self.engine.call(
                    "studio_message_send",
                    {
                        "reply_to": root,
                        "kind": "approval_answer",
                        "text": f"{'Approved' if approved else 'Denied'} {req.id} by {sender}"
                        + (f": {note}" if note else ""),
                        "data": {"request": req.id, "approved": approved, "by": sender, "note": note},
                    },
                )
                return Decision(approved=approved, by=sender, note=note)
        finally:
            await stream.close()


class ApprovalPolicy(BaseModel):
    """Which tool calls need approval. Checked in order: ``never``, ``always``, the tool's own flag,
    the role's brief (``ask`` access), then autonomy (``ask`` = every mutating call)."""

    autonomy: Literal["observe", "ask", "autonomous"] = "autonomous"
    always: list[str] = Field(default_factory=list)  # glob patterns of tool names
    never: list[str] = Field(default_factory=list)
    ask_tools: list[str] = Field(default_factory=list)  # from the role brief (access "ask")

    def needs_approval(self, tool: Tool) -> bool:
        name = tool.name
        if any(fnmatch.fnmatch(name, p) for p in self.never):
            return False
        if any(fnmatch.fnmatch(name, p) for p in self.always):
            return True
        if tool.requires_approval is not None:
            return tool.requires_approval
        if name in self.ask_tools:
            return True
        return self.autonomy == "ask" and tool.mutates

    def allowed(self, tool: Tool) -> bool:
        """Observe-mode agents only get read-only tools (memory and studio coordination excepted)."""
        return not (self.autonomy == "observe" and tool.mutates and tool.category not in ("studio", "memory"))
