"""Text embedders for semantic recall (optional: keyword search with FTS5 works without one)."""

from __future__ import annotations

import asyncio
import hashlib
import itertools
import math
import re
from typing import Any, Protocol, runtime_checkable


@runtime_checkable
class Embedder(Protocol):
    name: str
    dim: int

    async def embed(self, texts: list[str]) -> list[list[float]]: ...


def normalize(v: list[float]) -> list[float]:
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / n for x in v]


def cosine(a: list[float], b: list[float]) -> float:
    return sum(x * y for x, y in zip(a, b, strict=False))


class HashingEmbedder:
    """Dependency-free, deterministic bag-of-words embedder (feature hashing of words and bigrams).

    Good enough to rank paraphrases with shared words; use a real model for meaning.
    """

    def __init__(self, dim: int = 256) -> None:
        self.dim = dim
        self.name = f"hashing-{dim}"

    def _vector(self, text: str) -> list[float]:
        words = re.findall(r"[a-z0-9]+", text.lower())
        feats = words + [f"{a}_{b}" for a, b in itertools.pairwise(words)]
        v = [0.0] * self.dim
        for f in feats:
            h = int.from_bytes(hashlib.blake2b(f.encode(), digest_size=8).digest(), "little")
            v[h % self.dim] += 1.0 if (h >> 63) & 1 else -1.0
        return normalize(v)

    async def embed(self, texts: list[str]) -> list[list[float]]:
        return [self._vector(t) for t in texts]


class SentenceTransformerEmbedder:
    """Local embeddings with sentence-transformers (``[embeddings]`` extra)."""

    def __init__(self, model: str = "all-MiniLM-L6-v2") -> None:
        from sentence_transformers import SentenceTransformer

        self._model: Any = SentenceTransformer(model)
        self.name = f"st-{model}"
        self.dim = int(self._model.get_sentence_embedding_dimension())

    async def embed(self, texts: list[str]) -> list[list[float]]:
        vecs = await asyncio.to_thread(self._model.encode, texts, normalize_embeddings=True)
        return [list(map(float, v)) for v in vecs]


class OpenAIEmbedder:
    """Embeddings from any OpenAI-compatible ``/embeddings`` endpoint (``[openai]`` extra)."""

    def __init__(
        self,
        model: str = "text-embedding-3-small",
        *,
        dim: int = 1536,
        base_url: str | None = None,
        api_key: str | None = None,
        client: Any | None = None,
    ) -> None:
        if client is None:
            import openai

            client = openai.AsyncOpenAI(base_url=base_url, api_key=api_key)
        self.client = client
        self.model = model
        self.dim = dim
        self.name = f"openai-{model}"

    async def embed(self, texts: list[str]) -> list[list[float]]:
        resp = await self.client.embeddings.create(model=self.model, input=texts)
        return [normalize(list(map(float, d.embedding))) for d in resp.data]
