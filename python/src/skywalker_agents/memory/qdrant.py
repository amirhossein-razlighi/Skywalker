"""A Qdrant-backed memory store (``pip install skywalker-agents[qdrant]``): the reference external store.

Use it for large or shared deployments: ``QdrantMemoryStore(QdrantClient(url=...), embedder)``, or
``QdrantClient(":memory:")`` locally. It implements the same :class:`MemoryStore` protocol as SQLite,
so ``Memory(QdrantMemoryStore(...))`` works everywhere memory does. Ranking is vector similarity
blended with recency and importance (no keyword index).
"""

from __future__ import annotations

import math
import time
import uuid
from typing import Any

from .base import MemoryHit, MemoryItem, MemoryQuery
from .embeddings import Embedder


class QdrantMemoryStore:
    def __init__(self, client: Any, embedder: Embedder, *, collection: str = "skywalker_memory") -> None:
        from qdrant_client import models

        self.client = client
        self.embedder = embedder
        self.collection = collection
        self._m = models
        if not client.collection_exists(collection):
            client.create_collection(
                collection, vectors_config=models.VectorParams(size=embedder.dim, distance=models.Distance.COSINE)
            )

    @staticmethod
    def _pid(item_id: str) -> str:
        return str(uuid.uuid5(uuid.NAMESPACE_URL, item_id))

    async def add(self, item: MemoryItem) -> MemoryItem:
        vec = (await self.embedder.embed([f"{item.title}\n{item.text}".strip()]))[0]
        self.client.upsert(
            self.collection,
            points=[self._m.PointStruct(id=self._pid(item.id), vector=vec, payload=item.model_dump(mode="json"))],
        )
        return item

    async def get(self, item_id: str) -> MemoryItem | None:
        got = self.client.retrieve(self.collection, ids=[self._pid(item_id)], with_payload=True)
        return MemoryItem.model_validate(got[0].payload) if got else None

    async def update(self, item: MemoryItem) -> MemoryItem:
        item.updated_at = time.time()
        return await self.add(item)

    async def delete(self, item_ids: list[str]) -> int:
        existing = [i for i in item_ids if await self.get(i) is not None]
        if existing:
            self.client.delete(
                self.collection, points_selector=self._m.PointIdsList(points=[self._pid(i) for i in existing])
            )
        return len(existing)

    def _filter(self, q: MemoryQuery) -> Any:
        m = self._m
        must: list[Any] = []
        if not q.include_archived:
            must.append(m.FieldCondition(key="archived", match=m.MatchValue(value=False)))
        if q.kinds:
            must.append(m.FieldCondition(key="kind", match=m.MatchAny(any=list(q.kinds))))
        if q.scopes:
            must.append(m.FieldCondition(key="scope", match=m.MatchAny(any=list(q.scopes))))
        for t in q.tags or []:
            must.append(m.FieldCondition(key="tags", match=m.MatchValue(value=t)))
        should = None
        if q.visible is not None:
            should = []
            for scope, owner in q.visible:
                conds = [m.FieldCondition(key="scope", match=m.MatchValue(value=scope))]
                if owner != "*":
                    conds.append(m.FieldCondition(key="owner", match=m.MatchValue(value=owner)))
                should.append(m.Filter(must=conds))
        return m.Filter(must=must or None, should=should)

    async def search(self, query: MemoryQuery) -> list[MemoryHit]:
        if not query.text.strip():
            return [MemoryHit(item=i, score=i.importance) for i in await self.list_items(query)]
        vec = (await self.embedder.embed([query.text]))[0]
        res = self.client.query_points(
            self.collection, query=vec, query_filter=self._filter(query), limit=query.limit * 3, with_payload=True
        ).points
        hits = []
        for p in res:
            item = MemoryItem.model_validate(p.payload)
            recency = math.pow(0.5, max(0.0, time.time() - item.accessed_at) / 86400 / 14)
            why = {"vector": 0.8 * float(p.score), "recency": 0.1 * recency, "importance": 0.1 * item.importance}
            hits.append(MemoryHit(item=item, score=sum(why.values()), why=why))
        hits.sort(key=lambda h: h.score, reverse=True)
        return hits[: query.limit]

    async def list_items(self, query: MemoryQuery) -> list[MemoryItem]:
        points, _ = self.client.scroll(
            self.collection, scroll_filter=self._filter(query), limit=query.limit, with_payload=True
        )
        items = [MemoryItem.model_validate(p.payload) for p in points]
        return sorted(items, key=lambda i: i.created_at, reverse=True)

    async def touch(self, item_ids: list[str]) -> None:
        for i in item_ids:
            item = await self.get(i)
            if item is not None:
                item.accessed_at = time.time()
                item.access_count += 1
                self.client.set_payload(
                    self.collection,
                    payload={"accessed_at": item.accessed_at, "access_count": item.access_count},
                    points=[self._pid(i)],
                )

    async def stats(self) -> dict[str, Any]:
        return {"backend": "qdrant", "collection": self.collection, "total": self.client.count(self.collection).count}

    async def close(self) -> None:
        self.client.close()
