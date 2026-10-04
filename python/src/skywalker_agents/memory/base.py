"""Memory items, queries and the pluggable store interface."""

from __future__ import annotations

import secrets
import time
from typing import Any, Literal, Protocol, runtime_checkable

from pydantic import BaseModel, Field

Scope = Literal["agent", "team", "project", "global"]
Kind = Literal["episodic", "fact", "note", "artifact"]
SCOPES: tuple[Scope, ...] = ("agent", "team", "project", "global")
KINDS: tuple[Kind, ...] = ("episodic", "fact", "note", "artifact")


def new_id() -> str:
    return "mem_" + secrets.token_hex(6)


class MemoryItem(BaseModel):
    """One remembered thing.

    * scope ``agent`` is private to ``owner`` (an agent id); ``team`` is shared by a team (``owner`` is
      the team name); ``project`` by everyone working on the project; ``global`` follows the user
      across projects.
    * kind ``episodic`` is an event log entry (what happened), ``fact`` a durable statement,
      ``note`` free-form knowledge (with ``links`` to other items), ``artifact`` a file (``path``).
    """

    id: str = Field(default_factory=new_id)
    scope: Scope = "project"
    owner: str = ""
    kind: Kind = "note"
    title: str = ""
    text: str
    tags: list[str] = Field(default_factory=list)
    links: list[str] = Field(default_factory=list)
    source: str = ""
    importance: float = 0.5
    created_at: float = Field(default_factory=time.time)
    updated_at: float = Field(default_factory=time.time)
    accessed_at: float = Field(default_factory=time.time)
    access_count: int = 0
    expires_at: float | None = None
    path: str | None = None
    metadata: dict[str, Any] = Field(default_factory=dict)
    archived: bool = False

    def short(self, n: int = 160) -> str:
        head = f"{self.title}: " if self.title else ""
        body = self.text if len(self.text) <= n else self.text[:n] + "…"
        return head + body


class MemoryHit(BaseModel):
    item: MemoryItem
    score: float = 0.0
    why: dict[str, float] = Field(default_factory=dict)  # text, vector, recency, importance contributions


class MemoryQuery(BaseModel):
    text: str = ""
    scopes: list[Scope] | None = None
    # (scope, owner) pairs the reader may see; None = no restriction.
    visible: list[tuple[str, str]] | None = None
    kinds: list[Kind] | None = None
    tags: list[str] | None = None
    source: str | None = None
    limit: int = 8
    include_archived: bool = False
    since: float | None = None


@runtime_checkable
class MemoryStore(Protocol):
    """Where memory lives. SQLite (+FTS5, optional vectors) by default; Qdrant/Chroma or your own via plugins."""

    async def add(self, item: MemoryItem) -> MemoryItem: ...

    async def get(self, item_id: str) -> MemoryItem | None: ...

    async def update(self, item: MemoryItem) -> MemoryItem: ...

    async def delete(self, item_ids: list[str]) -> int: ...

    async def search(self, query: MemoryQuery) -> list[MemoryHit]: ...

    async def list_items(self, query: MemoryQuery) -> list[MemoryItem]: ...

    async def touch(self, item_ids: list[str]) -> None: ...

    async def stats(self) -> dict[str, Any]: ...

    async def close(self) -> None: ...
