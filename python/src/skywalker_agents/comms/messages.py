"""Structured messages between agents (stored as studio messages: ``kind`` + ``data``)."""

from __future__ import annotations

from typing import Any

from pydantic import BaseModel, Field

# Conventional kinds; any slug works.
CHAT = "chat"
REQUEST = "request"
INFORM = "inform"
HANDOFF = "handoff"
RESULT = "result"
QUESTION = "question"
ANSWER = "answer"
APPROVAL_REQUEST = "approval_request"
APPROVAL_ANSWER = "approval_answer"


class Envelope(BaseModel):
    """One message. ``topic`` is the studio channel; ``thread`` the root message id of a conversation."""

    id: str = ""
    kind: str = CHAT
    sender: str = ""
    to: list[str] = Field(default_factory=list)
    mentions: list[str] = Field(default_factory=list)
    topic: str = "general"
    thread: str = ""
    reply_to: str = ""
    text: str
    data: dict[str, Any] = Field(default_factory=dict)
    refs: dict[str, Any] = Field(default_factory=dict)
    at: str = ""

    @property
    def root(self) -> str:
        """The thread this message belongs to (itself when it starts one)."""
        return self.thread or self.id

    def addressed_to(self, agent: str) -> bool:
        return agent in self.to or agent in self.mentions

    @classmethod
    def from_studio(cls, m: dict[str, Any]) -> Envelope:
        return cls(
            id=str(m.get("id", "")),
            kind=str(m.get("kind") or CHAT),
            sender=str(m.get("from", "")),
            to=list(m.get("to") or []),
            mentions=list(m.get("mentions") or []),
            topic=str(m.get("channel") or "general"),
            thread=str(m.get("thread") or ""),
            text=str(m.get("text", "")),
            data=dict(m.get("data") or {}),
            refs=dict(m.get("refs") or {}),
            at=str(m.get("at", "")),
        )

    def to_studio_args(self) -> dict[str, Any]:
        args: dict[str, Any] = {"text": self.text}
        if self.to:
            args["to"] = list(self.to)
        if self.topic and self.topic != "general":
            args["channel"] = self.topic
        if self.reply_to:
            args["reply_to"] = self.reply_to
        if self.kind and self.kind != CHAT:
            args["kind"] = self.kind
        if self.data:
            args["data"] = self.data
        for k in ("task", "feedback"):
            if k in self.refs:
                args[k] = self.refs[k]
        return args
