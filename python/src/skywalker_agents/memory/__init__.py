"""Shared memory: scopes (agent, team, project, global), kinds (episodic, fact, note, artifact),
SQLite + FTS5 by default with optional vectors, pluggable stores."""

from .base import KINDS, SCOPES, Kind, MemoryHit, MemoryItem, MemoryQuery, MemoryStore, Scope
from .embeddings import Embedder, HashingEmbedder, OpenAIEmbedder, SentenceTransformerEmbedder
from .memory import Memory, home, llm_summarizer, project_memory_path
from .sqlite import SQLiteMemoryStore
from .tools import memory_tools

__all__ = [
    "KINDS",
    "SCOPES",
    "Embedder",
    "HashingEmbedder",
    "Kind",
    "Memory",
    "MemoryHit",
    "MemoryItem",
    "MemoryQuery",
    "MemoryStore",
    "OpenAIEmbedder",
    "SQLiteMemoryStore",
    "Scope",
    "SentenceTransformerEmbedder",
    "home",
    "llm_summarizer",
    "memory_tools",
    "project_memory_path",
]
