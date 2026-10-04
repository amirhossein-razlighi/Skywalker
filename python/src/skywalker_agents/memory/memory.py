"""Shared memory for agents: what an agent (or its team, the project, the user) remembers.

    memory = Memory.open("examples/sky_dash", agent="mira", teams=["level"])
    await memory.remember("The lava bridge must stay 3 m wide", kind="fact", scope="project", tags=["level"])
    hits = await memory.recall("bridge width")

Project and team memories live in ``<project>/studio/memory.sqlite`` (shared by every agent and
tool on the project, including Claude Code through ``sky-agents serve-mcp`` or ``py_memory_*``
tools); global memories in ``~/.skywalker/agents/memory.sqlite``.
"""

from __future__ import annotations

import os
import re
import time
from collections import defaultdict
from collections.abc import Awaitable, Callable, Sequence
from pathlib import Path
from typing import TYPE_CHECKING, Any

from .base import KINDS, SCOPES, Kind, MemoryHit, MemoryItem, MemoryQuery, MemoryStore, Scope
from .embeddings import Embedder
from .sqlite import SQLiteMemoryStore

if TYPE_CHECKING:  # pragma: no cover
    from ..engine.client import AgentSession
    from ..llm.types import Provider
    from ..observability.tracing import Tracer
    from ..tools.base import Tool

Summarizer = Callable[[str, list[MemoryItem]], Awaitable[str]]


def home() -> Path:
    """Per-user state: ``$SKY_AGENTS_HOME`` or ``~/.skywalker/agents``."""
    return Path(os.environ.get("SKY_AGENTS_HOME") or Path.home() / ".skywalker" / "agents")


def project_memory_path(project: str | os.PathLike[str]) -> Path:
    return Path(project) / "studio" / "memory.sqlite"


class Memory:
    """A reader/writer's view of memory: who they are decides what they see."""

    def __init__(
        self,
        store: MemoryStore,
        *,
        agent: str = "",
        teams: Sequence[str] = (),
        global_store: MemoryStore | None = None,
        session: AgentSession | None = None,
        tracer: Tracer | None = None,
    ) -> None:
        self.store = store
        self.global_store = global_store
        self.agent = agent
        self.teams = list(teams)
        self.session = session
        self.tracer = tracer

    @classmethod
    def open(
        cls,
        project: str | os.PathLike[str] | None = None,
        *,
        agent: str = "",
        teams: Sequence[str] = (),
        embedder: Embedder | None = None,
        global_memory: bool = True,
        path: str | None = None,
    ) -> Memory:
        """SQLite memory for a project (``:memory:`` when neither project nor path is given)."""
        store_path = path or (str(project_memory_path(project)) if project else ":memory:")
        store = SQLiteMemoryStore(store_path, embedder=embedder)
        global_store = None
        if global_memory and store_path != ":memory:":
            global_store = SQLiteMemoryStore(home() / "memory.sqlite", embedder=embedder)
        return cls(store, agent=agent, teams=teams, global_store=global_store)

    def for_agent(self, agent: str, teams: Sequence[str] = (), session: AgentSession | None = None) -> Memory:
        """The same stores seen by another agent."""
        return Memory(
            self.store,
            agent=agent,
            teams=teams or self.teams,
            global_store=self.global_store,
            session=session or self.session,
            tracer=self.tracer,
        )

    # ------------------------------------------------------------------ visibility
    def visible(self) -> list[tuple[str, str]]:
        vis: list[tuple[str, str]] = [("project", "*")]
        if self.agent:
            vis.append(("agent", self.agent))
        vis += [("team", t) for t in self.teams]
        return vis

    def _store_for(self, scope: str) -> MemoryStore:
        if scope == "global":
            if self.global_store is None:
                raise ValueError("global memory is not enabled for this Memory (open it with a project)")
            return self.global_store
        return self.store

    def _owner(self, scope: Scope, team: str | None) -> str:
        if scope == "agent":
            if not self.agent:
                raise ValueError("agent-scoped memory needs an agent id (Memory(..., agent='mira'))")
            return self.agent
        if scope == "team":
            owner = team or (self.teams[0] if self.teams else "")
            if not owner:
                raise ValueError("team-scoped memory needs a team (teams=[...] or team='...')")
            return owner
        return ""

    # ------------------------------------------------------------------ write
    async def remember(
        self,
        text: str,
        *,
        kind: Kind = "note",
        scope: Scope = "agent",
        title: str = "",
        tags: Sequence[str] = (),
        links: Sequence[str] = (),
        importance: float = 0.5,
        ttl_days: float | None = None,
        path: str | None = None,
        team: str | None = None,
        metadata: dict[str, Any] | None = None,
        pin: bool = False,
        dedupe: bool = True,
    ) -> MemoryItem:
        """Stores something. ``pin=True`` also adds it to the agent's studio notes (shown in its brief)."""
        if kind not in KINDS:
            raise ValueError(f"kind must be one of {KINDS}")
        if scope not in SCOPES:
            raise ValueError(f"scope must be one of {SCOPES}")
        text = text.strip()
        if not text:
            raise ValueError("nothing to remember: text is empty")
        owner = self._owner(scope, team)
        store = self._store_for(scope)
        if dedupe and kind in ("fact", "note"):
            dup = await self._find_duplicate(store, text, scope, owner)
            if dup is not None:
                dup.tags = sorted(set(dup.tags) | set(tags))
                dup.importance = max(dup.importance, importance)
                dup.access_count += 1
                dup.accessed_at = time.time()
                return await store.update(dup)
        item = MemoryItem(
            scope=scope,
            owner=owner,
            kind=kind,
            title=title,
            text=text,
            tags=list(tags),
            links=list(links),
            source=self.agent,
            importance=max(0.0, min(1.0, importance)),
            expires_at=time.time() + ttl_days * 86400 if ttl_days else None,
            path=path,
            metadata=dict(metadata or {}),
        )
        await store.add(item)
        for other in links:
            await self._backlink(other, item.id)
        if pin and self.session is not None:
            await self.session.call("studio_memory", {"action": "note", "text": item.short(300)})
        return item

    async def log(self, text: str, **kwargs: Any) -> MemoryItem:
        """An episodic entry (what happened), private to the agent by default."""
        kwargs.setdefault("scope", "agent" if self.agent else "project")
        return await self.remember(text, kind="episodic", dedupe=False, **kwargs)

    async def add_artifact(
        self, path: str, description: str, *, scope: Scope = "project", tags: Sequence[str] = (), **kwargs: Any
    ) -> MemoryItem:
        """Remembers a file (capture, report, exported asset) with what it shows."""
        return await self.remember(
            description,
            kind="artifact",
            scope=scope,
            path=path,
            tags=tags,
            title=Path(path).name,
            dedupe=False,
            **kwargs,
        )

    async def link(self, a: str, b: str) -> None:
        for x, y in ((a, b), (b, a)):
            item = await self.get(x)
            if item is not None and y not in item.links:
                item.links.append(y)
                await self._store_for(item.scope).update(item)

    async def _backlink(self, target: str, source: str) -> None:
        item = await self.get(target)
        if item is not None and source not in item.links:
            item.links.append(source)
            await self._store_for(item.scope).update(item)

    async def _find_duplicate(self, store: MemoryStore, text: str, scope: str, owner: str) -> MemoryItem | None:
        norm = _norm(text)
        q = MemoryQuery(text=text, visible=[(scope, owner or "*")], limit=5)
        for hit in await store.search(q):
            if _norm(hit.item.text) == norm:
                return hit.item
        return None

    # ------------------------------------------------------------------ read
    async def get(self, item_id: str) -> MemoryItem | None:
        item = await self.store.get(item_id)
        if item is None and self.global_store is not None:
            item = await self.global_store.get(item_id)
        return item

    async def recall(
        self,
        query: str = "",
        *,
        scopes: Sequence[Scope] | None = None,
        kinds: Sequence[Kind] | None = None,
        tags: Sequence[str] | None = None,
        k: int = 8,
        include_archived: bool = False,
    ) -> list[MemoryHit]:
        """The ``k`` most relevant memories this reader may see (keyword + semantic + recency + importance)."""
        wanted = list(scopes or SCOPES)
        q = MemoryQuery(
            text=query,
            visible=[v for v in self.visible() if v[0] in wanted],
            kinds=list(kinds) if kinds else None,
            tags=list(tags) if tags else None,
            limit=k,
            include_archived=include_archived,
        )
        hits: list[MemoryHit] = []
        if q.visible:
            hits += await self.store.search(q)
        if "global" in wanted and self.global_store is not None:
            hits += await self.global_store.search(q.model_copy(update={"visible": [("global", "*")]}))
        hits.sort(key=lambda h: h.score, reverse=True)
        hits = hits[:k]
        for store in {id(self.store): self.store, id(self.global_store): self.global_store}.values():
            if store is not None:
                await store.touch([h.item.id for h in hits])
        return hits

    async def forget(
        self, item_id: str | None = None, *, query: str | None = None, scope: Scope | None = None, limit: int = 1
    ) -> int:
        """Deletes by id, or the best matches of ``query`` (only items this reader may see)."""
        if item_id:
            item = await self.get(item_id)
            if item is None or not self._can_see(item):
                return 0
            return await self._store_for(item.scope).delete([item_id])
        if not query:
            raise ValueError("forget needs an id or a query")
        hits = await self.recall(query, scopes=[scope] if scope else None, k=limit)
        n = 0
        for h in hits:
            n += await self._store_for(h.item.scope).delete([h.item.id])
        return n

    def _can_see(self, item: MemoryItem) -> bool:
        if item.scope in ("project", "global"):
            return True
        return (item.scope, item.owner) in self.visible()

    # ------------------------------------------------------------------ digest
    async def summarize(
        self,
        query: str = "",
        *,
        scopes: Sequence[Scope] | None = None,
        kinds: Sequence[Kind] | None = None,
        tags: Sequence[str] | None = None,
        limit: int = 40,
        summarizer: Summarizer | None = None,
    ) -> str:
        """A digest of matching memories: by an LLM ``summarizer`` when given, else a grouped outline."""
        hits = await self.recall(query, scopes=scopes, kinds=kinds, tags=tags, k=limit)
        items = [h.item for h in hits]
        if not items:
            return "(nothing remembered yet)"
        if summarizer is not None:
            return await summarizer(query, items)
        groups: dict[str, list[MemoryItem]] = defaultdict(list)
        for it in items:
            groups[f"{it.kind} ({it.scope}{'/' + it.owner if it.owner else ''})"].append(it)
        lines = []
        for g, its in groups.items():
            lines.append(f"{g}:")
            lines += [f"  - [{i.id}] {i.short(200)}" + (f"  #{' #'.join(i.tags)}" if i.tags else "") for i in its]
        return "\n".join(lines)

    def context_block(self, hits: Sequence[MemoryHit], title: str = "Relevant memory") -> str:
        """Memories formatted for a prompt."""
        if not hits:
            return ""
        lines = [f"## {title}"]
        for h in hits:
            it = h.item
            where = it.scope if not it.owner else f"{it.scope}:{it.owner}"
            lines.append(f"- ({it.kind}, {where}, {it.id}) {it.short(300)}")
        return "\n".join(lines)

    # ------------------------------------------------------------------ maintenance
    async def consolidate(
        self,
        *,
        older_than_days: float = 1.0,
        min_group: int = 3,
        summarizer: Summarizer | None = None,
        scope: Scope | None = None,
    ) -> dict[str, int]:
        """Folds old episodic entries into one note per (scope, owner, tag) and archives the originals;
        merges duplicate facts. Returns counts."""
        cutoff = time.time() - older_than_days * 86400
        stats = {"summaries": 0, "archived": 0, "merged": 0}
        for store, vis in ((self.store, self.visible()), (self.global_store, [("global", "*")])):
            if store is None:
                continue
            items = await store.list_items(MemoryQuery(visible=vis, limit=100000))
            if scope:
                items = [i for i in items if i.scope == scope]
            groups: dict[tuple[str, str, str], list[MemoryItem]] = defaultdict(list)
            for it in items:
                if it.kind == "episodic" and it.created_at < cutoff:
                    groups[(it.scope, it.owner, it.tags[0] if it.tags else "")].append(it)
            for (sc, owner, tag), its in groups.items():
                if len(its) < min_group:
                    continue
                its.sort(key=lambda i: i.created_at)
                if summarizer is not None:
                    text = await summarizer(f"Summarize these {len(its)} events", its)
                else:
                    text = "; ".join(i.short(120) for i in its)
                note = MemoryItem(
                    scope=sc,
                    owner=owner,
                    kind="note",
                    title=f"Summary of {len(its)} events" + (f" ({tag})" if tag else ""),
                    text=text,
                    tags=[tag] if tag else [],
                    links=[i.id for i in its],
                    source=self.agent or "consolidation",
                    importance=max(i.importance for i in its),
                )
                await store.add(note)
                for i in its:
                    i.archived = True
                    i.links = [*i.links, note.id]
                    await store.update(i)
                stats["summaries"] += 1
                stats["archived"] += len(its)
            seen: dict[tuple[str, str, str], MemoryItem] = {}
            for it in sorted(items, key=lambda i: i.created_at):
                if it.kind != "fact" or it.archived:
                    continue
                key = (it.scope, it.owner, _norm(it.text))
                if key in seen:
                    keep = seen[key]
                    keep.tags = sorted(set(keep.tags) | set(it.tags))
                    keep.access_count += it.access_count
                    keep.importance = max(keep.importance, it.importance)
                    await store.update(keep)
                    await store.delete([it.id])
                    stats["merged"] += 1
                else:
                    seen[key] = it
        return stats

    async def decay(
        self, *, half_life_days: float = 30.0, threshold: float = 0.05, keep_importance: float = 0.9
    ) -> int:
        """Archives memories whose strength (importance x recency of use x use count) fell below
        ``threshold``. Items at or above ``keep_importance`` never decay. Returns how many were archived."""
        archived = 0
        now = time.time()
        for store, vis in ((self.store, self.visible()), (self.global_store, [("global", "*")])):
            if store is None:
                continue
            for it in await store.list_items(MemoryQuery(visible=vis, limit=100000)):
                if it.importance >= keep_importance:
                    continue
                age = max(0.0, now - it.accessed_at) / 86400
                strength = it.importance * 0.5 ** (age / half_life_days) * (1 + 0.25 * it.access_count)
                if strength < threshold:
                    it.archived = True
                    await store.update(it)
                    archived += 1
        return archived

    async def stats(self) -> dict[str, Any]:
        out = {"project": await self.store.stats()}
        if self.global_store is not None:
            out["global"] = await self.global_store.stats()
        return out

    def tools(self, *, prefix: str = "memory_") -> list[Tool]:
        """remember / recall / forget / summarize as agent tools bound to this reader."""
        from .tools import memory_tools

        return memory_tools(self, prefix=prefix)

    async def close(self) -> None:
        await self.store.close()
        if self.global_store is not None:
            await self.global_store.close()


def _norm(text: str) -> str:
    return re.sub(r"\W+", " ", text.lower()).strip()


def llm_summarizer(provider: Provider, model: str = "") -> Summarizer:
    """A summarizer that asks a model to digest memories (for summarize / consolidate)."""
    from ..llm.types import ChatMessage, LLMRequest

    async def summarize(prompt: str, items: list[MemoryItem]) -> str:
        listing = "\n".join(f"- [{i.id}] ({i.kind}) {i.short(400)}" for i in items)
        req = LLMRequest(
            model=model or provider.default_model,
            system="You condense an agent team's memory into "
            "a short, faithful digest. Keep ids in brackets for facts you rely on. No speculation.",
            messages=[ChatMessage.user(f"{prompt or 'Summarize'}:\n{listing}")],
            max_tokens=2000,
            effort="low",
        )
        return (await provider.complete(req)).text

    return summarize
