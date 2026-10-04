"""The default memory store: one SQLite file with FTS5 keyword search and optional vectors.

Vectors use `sqlite-vec <https://github.com/asg017/sqlite-vec>`_ when installed (``[memory-vector]``
extra) and fall back to an exact in-Python cosine scan otherwise. Ranking blends keyword relevance,
semantic similarity, recency and importance.
"""

from __future__ import annotations

import json
import math
import re
import sqlite3
import struct
import threading
import time
from pathlib import Path
from typing import Any

from .base import MemoryHit, MemoryItem, MemoryQuery
from .embeddings import Embedder, cosine

_SCHEMA = """
create table if not exists items(
    id text primary key, scope text not null, owner text not null default '', kind text not null,
    title text not null default '', text text not null, tags text not null default '[]',
    links text not null default '[]', source text not null default '', importance real not null default 0.5,
    created_at real not null, updated_at real not null, accessed_at real not null,
    access_count integer not null default 0, expires_at real, path text, metadata text not null default '{}',
    archived integer not null default 0, embedding blob, embed_model text
);
create index if not exists items_scope on items(scope, owner);
create index if not exists items_kind on items(kind);
create virtual table if not exists items_fts using fts5(
    title, text, tags, content='items', content_rowid='rowid', tokenize='porter unicode61');
create trigger if not exists items_ai after insert on items begin
    insert into items_fts(rowid, title, text, tags) values (new.rowid, new.title, new.text, new.tags);
end;
create trigger if not exists items_ad after delete on items begin
    insert into items_fts(items_fts, rowid, title, text, tags)
        values ('delete', old.rowid, old.title, old.text, old.tags);
end;
create trigger if not exists items_au after update of title, text, tags on items begin
    insert into items_fts(items_fts, rowid, title, text, tags)
        values ('delete', old.rowid, old.title, old.text, old.tags);
    insert into items_fts(rowid, title, text, tags) values (new.rowid, new.title, new.text, new.tags);
end;
create table if not exists meta(key text primary key, value text);
"""

_COLUMNS = (
    "id",
    "scope",
    "owner",
    "kind",
    "title",
    "text",
    "tags",
    "links",
    "source",
    "importance",
    "created_at",
    "updated_at",
    "accessed_at",
    "access_count",
    "expires_at",
    "path",
    "metadata",
    "archived",
)


def _pack(v: list[float]) -> bytes:
    return struct.pack(f"{len(v)}f", *v)


def _unpack(b: bytes) -> list[float]:
    return list(struct.unpack(f"{len(b) // 4}f", b))


def fts_query(text: str) -> str:
    """User text -> a safe FTS5 query (quoted terms OR-ed; prefix match on the last term)."""
    words = re.findall(r"[\w']+", text.lower())
    words = [w.replace("'", "") for w in words if len(w) > 1 or w.isdigit()]
    if not words:
        return ""
    terms = [f'"{w}"' for w in words[:-1]] + [f'"{words[-1]}"*']
    return " OR ".join(terms)


class SQLiteMemoryStore:
    """Thread-safe SQLite memory. ``path=":memory:"`` for tests."""

    def __init__(
        self,
        path: str | Path = ":memory:",
        *,
        embedder: Embedder | None = None,
        weights: dict[str, float] | None = None,
        use_sqlite_vec: bool | None = None,
    ) -> None:
        self.path = str(path)
        if self.path != ":memory:":
            Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        self._db = sqlite3.connect(self.path, check_same_thread=False, isolation_level=None)
        self._db.row_factory = sqlite3.Row
        self._lock = threading.RLock()
        with self._lock:
            self._db.execute("pragma journal_mode=wal" if self.path != ":memory:" else "pragma journal_mode=memory")
            self._db.execute("pragma busy_timeout=5000")
            self._db.executescript(_SCHEMA)
        self.embedder = embedder
        self.weights = {"text": 0.45, "vector": 0.35, "recency": 0.1, "importance": 0.1, **(weights or {})}
        self.vec_enabled = False
        if embedder is not None and use_sqlite_vec is not False:
            self.vec_enabled = self._enable_vec(embedder.dim)

    def _enable_vec(self, dim: int) -> bool:
        try:
            import sqlite_vec

            with self._lock:
                self._db.enable_load_extension(True)
                sqlite_vec.load(self._db)
                self._db.enable_load_extension(False)
                self._db.execute(f"create virtual table if not exists items_vec using vec0(embedding float[{dim}])")
            return True
        except Exception:
            return False

    # ------------------------------------------------------------------ rows
    @staticmethod
    def _row(item: MemoryItem) -> dict[str, Any]:
        d = item.model_dump()
        d["tags"] = json.dumps(item.tags)
        d["links"] = json.dumps(item.links)
        d["metadata"] = json.dumps(item.metadata, default=str)
        d["archived"] = int(item.archived)
        return {k: d[k] for k in _COLUMNS}

    @staticmethod
    def _item(row: sqlite3.Row) -> MemoryItem:
        d = {k: row[k] for k in _COLUMNS}
        d["tags"] = json.loads(d["tags"] or "[]")
        d["links"] = json.loads(d["links"] or "[]")
        d["metadata"] = json.loads(d["metadata"] or "{}")
        d["archived"] = bool(d["archived"])
        return MemoryItem.model_validate(d)

    async def _embed(self, item: MemoryItem) -> bytes | None:
        if self.embedder is None:
            return None
        vec = (await self.embedder.embed([f"{item.title}\n{item.text}".strip()]))[0]
        return _pack(vec)

    # ------------------------------------------------------------------ CRUD
    async def add(self, item: MemoryItem) -> MemoryItem:
        emb = await self._embed(item)
        row = self._row(item)
        cols = ", ".join(_COLUMNS)
        marks = ", ".join(f":{c}" for c in _COLUMNS)
        with self._lock:
            # Delete first (not INSERT OR REPLACE): REPLACE does not fire the FTS delete trigger.
            if self.vec_enabled:
                self._db.execute(
                    "delete from items_vec where rowid in (select rowid from items where id=?)", (item.id,)
                )
            self._db.execute("delete from items where id=?", (item.id,))
            cur = self._db.execute(f"insert into items({cols}) values ({marks})", row)
            rowid = cur.lastrowid
            if emb is not None:
                self._db.execute(
                    "update items set embedding=?, embed_model=? where id=?",
                    (emb, self.embedder.name if self.embedder else None, item.id),
                )
                if self.vec_enabled:
                    self._db.execute("insert into items_vec(rowid, embedding) values (?, ?)", (rowid, emb))
        return item

    async def get(self, item_id: str) -> MemoryItem | None:
        with self._lock:
            row = self._db.execute("select * from items where id=?", (item_id,)).fetchone()
        return self._item(row) if row else None

    async def update(self, item: MemoryItem) -> MemoryItem:
        item.updated_at = time.time()
        existing = await self.get(item.id)
        if existing is None or existing.text != item.text or existing.title != item.title:
            return await self.add(item)  # re-embed
        row = self._row(item)
        sets = ", ".join(f"{c}=:{c}" for c in _COLUMNS if c != "id")
        with self._lock:
            self._db.execute(f"update items set {sets} where id=:id", row)
        return item

    async def delete(self, item_ids: list[str]) -> int:
        if not item_ids:
            return 0
        marks = ",".join("?" * len(item_ids))
        with self._lock:
            if self.vec_enabled:
                self._db.execute(
                    f"delete from items_vec where rowid in (select rowid from items where id in ({marks}))", item_ids
                )
            cur = self._db.execute(f"delete from items where id in ({marks})", item_ids)
        return cur.rowcount

    async def touch(self, item_ids: list[str]) -> None:
        if not item_ids:
            return
        marks = ",".join("?" * len(item_ids))
        with self._lock:
            self._db.execute(
                f"update items set accessed_at=?, access_count=access_count+1 where id in ({marks})",
                [time.time(), *item_ids],
            )

    # ------------------------------------------------------------------ queries
    def _where(self, q: MemoryQuery) -> tuple[str, list[Any]]:
        clauses: list[str] = []
        params: list[Any] = []
        if not q.include_archived:
            clauses.append("i.archived=0")
        clauses.append("(i.expires_at is null or i.expires_at > ?)")
        params.append(time.time())
        if q.scopes:
            clauses.append(f"i.scope in ({','.join('?' * len(q.scopes))})")
            params += list(q.scopes)
        if q.visible is not None:
            ors = []
            for scope, owner in q.visible:
                if owner == "*":
                    ors.append("i.scope=?")
                    params.append(scope)
                else:
                    ors.append("(i.scope=? and i.owner=?)")
                    params += [scope, owner]
            clauses.append("(" + (" or ".join(ors) or "0") + ")")
        if q.kinds:
            clauses.append(f"i.kind in ({','.join('?' * len(q.kinds))})")
            params += list(q.kinds)
        if q.source:
            clauses.append("i.source=?")
            params.append(q.source)
        if q.since is not None:
            clauses.append("i.created_at >= ?")
            params.append(q.since)
        for tag in q.tags or []:
            clauses.append("exists (select 1 from json_each(i.tags) where value=?)")
            params.append(tag)
        return " and ".join(clauses) or "1", params

    async def list_items(self, query: MemoryQuery) -> list[MemoryItem]:
        where, params = self._where(query)
        with self._lock:
            rows = self._db.execute(
                f"select * from items i where {where} order by i.created_at desc limit ?", [*params, query.limit]
            ).fetchall()
        return [self._item(r) for r in rows]

    async def search(self, query: MemoryQuery) -> list[MemoryHit]:
        if not query.text.strip():
            items = await self.list_items(query)
            return [MemoryHit(item=i, score=self._prior(i), why={"recency": self._recency(i)}) for i in items]
        where, params = self._where(query)
        pool = max(query.limit * 8, 50)
        text_rank: dict[str, float] = {}
        fq = fts_query(query.text)
        with self._lock:
            if fq:
                rows = self._db.execute(
                    f"select i.id from items_fts f join items i on i.rowid=f.rowid where items_fts match ? and {where} "
                    f"order by bm25(items_fts, 2.0, 1.0, 1.5) limit ?",
                    [fq, *params, pool],
                ).fetchall()
                n = len(rows)
                for rank, r in enumerate(rows):
                    text_rank[r["id"]] = 1.0 - rank / max(n, 1) * 0.5  # 1.0 .. 0.5 by rank
        vec_score: dict[str, float] = {}
        if self.embedder is not None:
            qv = (await self.embedder.embed([query.text]))[0]
            with self._lock:
                if self.vec_enabled:
                    rows = self._db.execute(
                        f"select i.id, i.embedding from (select rowid, distance from items_vec where embedding match ? "
                        f"and k = ?) v join items i on i.rowid=v.rowid where {where}",
                        [_pack(qv), pool, *params],
                    ).fetchall()
                else:
                    rows = self._db.execute(
                        f"select i.id, i.embedding from items i where i.embedding is not null and {where}", params
                    ).fetchall()
            for r in rows:
                sim = cosine(qv, _unpack(r["embedding"]))
                if sim > 0.05:
                    vec_score[r["id"]] = sim
        ids = list(dict.fromkeys([*text_rank, *sorted(vec_score, key=vec_score.__getitem__, reverse=True)[:pool]]))
        if not ids:
            return []
        marks = ",".join("?" * len(ids))
        with self._lock:
            rows = self._db.execute(f"select * from items where id in ({marks})", ids).fetchall()
        w = self.weights
        hits = []
        for r in rows:
            item = self._item(r)
            why = {
                "text": text_rank.get(item.id, 0.0) * w["text"],
                "vector": vec_score.get(item.id, 0.0) * w["vector"],
                "recency": self._recency(item) * w["recency"],
                "importance": item.importance * w["importance"],
            }
            hits.append(MemoryHit(item=item, score=sum(why.values()), why=why))
        hits.sort(key=lambda h: h.score, reverse=True)
        return hits[: query.limit]

    @staticmethod
    def _recency(item: MemoryItem, half_life_days: float = 14.0) -> float:
        age_days = max(0.0, time.time() - item.accessed_at) / 86400
        return math.pow(0.5, age_days / half_life_days)

    def _prior(self, item: MemoryItem) -> float:
        return 0.5 * self._recency(item) + 0.5 * item.importance

    async def stats(self) -> dict[str, Any]:
        with self._lock:
            rows = self._db.execute(
                "select scope, kind, archived, count(*) n from items group by scope, kind, archived"
            ).fetchall()
        out: dict[str, Any] = {
            "path": self.path,
            "vector": self.vec_enabled or self.embedder is not None,
            "sqlite_vec": self.vec_enabled,
            "embedder": getattr(self.embedder, "name", None),
            "total": 0,
            "archived": 0,
            "by_scope": {},
            "by_kind": {},
        }
        for r in rows:
            out["total"] += r["n"]
            if r["archived"]:
                out["archived"] += r["n"]
            out["by_scope"][r["scope"]] = out["by_scope"].get(r["scope"], 0) + r["n"]
            out["by_kind"][r["kind"]] = out["by_kind"].get(r["kind"], 0) + r["n"]
        return out

    async def all_items(self, include_archived: bool = True) -> list[MemoryItem]:
        with self._lock:
            rows = self._db.execute(
                "select * from items" + ("" if include_archived else " where archived=0") + " order by created_at"
            ).fetchall()
        return [self._item(r) for r in rows]

    async def close(self) -> None:
        with self._lock:
            self._db.close()
