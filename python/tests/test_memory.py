from __future__ import annotations

import time
from pathlib import Path

import pytest

from skywalker_agents.memory import HashingEmbedder, Memory, MemoryQuery, SQLiteMemoryStore, memory_tools
from skywalker_agents.memory.sqlite import fts_query
from skywalker_agents.tools import ToolContext


async def test_scopes_visibility_and_keyword_search(project: Path) -> None:
    base = Memory.open(project, agent="mira", teams=["level"])
    mira, ash = base, base.for_agent("ash", teams=["art"])
    await mira.remember("Secret room behind the waterfall", kind="note", scope="agent")
    await mira.remember("Level team: ramps max 30 degrees", kind="fact", scope="team")
    await ash.remember("Palette: warm oranges for lava", kind="fact", scope="team")
    await mira.remember("The game targets 60 fps on M1", kind="fact", scope="project")
    await mira.remember("The user prefers short commit messages", kind="fact", scope="global")

    seen = {h.item.text for h in await mira.recall("", k=20)}
    assert "Secret room behind the waterfall" in seen and "Palette: warm oranges for lava" not in seen
    assert "The user prefers short commit messages" in seen
    assert not await ash.recall("waterfall secret")
    hits = await ash.recall("lava palette")
    assert hits[0].item.text.startswith("Palette") and hits[0].why["text"] > 0
    assert (project / "studio" / "memory.sqlite").exists()
    stats = await mira.stats()
    assert stats["project"]["total"] == 4 and stats["global"]["total"] == 1
    with pytest.raises(ValueError, match="team"):
        await Memory.open(project).remember("x", scope="team")
    await base.close()


async def test_dedupe_links_forget_and_ttl() -> None:
    mem = Memory(SQLiteMemoryStore(), agent="mira")
    a = await mem.remember("Bridges are 3 m wide", kind="fact", scope="project", tags=["level"])
    b = await mem.remember("bridges are 3 m wide!", kind="fact", scope="project", tags=["design"], importance=0.9)
    assert a.id == b.id and set(b.tags) == {"level", "design"} and b.importance == 0.9
    note = await mem.remember("Why: players kept falling", kind="note", scope="project", links=[a.id])
    linked = await mem.get(a.id)
    assert linked is not None and note.id in linked.links
    await mem.remember("temporary", kind="note", scope="project", ttl_days=-1)  # already expired
    assert all(h.item.text != "temporary" for h in await mem.recall("temporary"))
    assert await mem.forget(query="players kept falling") == 1
    assert await mem.forget(a.id) == 1
    assert await mem.recall("bridges") == []


async def test_semantic_recall_with_embeddings() -> None:
    store = SQLiteMemoryStore(embedder=HashingEmbedder(), use_sqlite_vec=False)
    mem = Memory(store, agent="mira")
    await mem.remember("enemy patrol routes loop around the tower", scope="project")
    await mem.remember("coin pickups glow yellow", scope="project")
    hits = await mem.recall("tower patrol", k=1)
    assert hits[0].item.text.startswith("enemy patrol") and hits[0].why["vector"] > 0


async def test_sqlite_vec_backend_when_available() -> None:
    pytest.importorskip("sqlite_vec")
    store = SQLiteMemoryStore(embedder=HashingEmbedder())
    if not store.vec_enabled:
        pytest.skip("this Python's sqlite3 cannot load extensions")
    mem = Memory(store)
    await mem.remember("the boss arena has four pillars", scope="project")
    await mem.remember("menus use a serif font", scope="project")
    assert (await mem.recall("pillars in the arena", k=1))[0].item.text.startswith("the boss")


async def test_consolidation_and_decay() -> None:
    mem = Memory(SQLiteMemoryStore(), agent="mira")
    for i in range(4):
        item = await mem.log(f"playtest {i}: died at the bridge", tags=["playtest"])
        item.created_at = time.time() - 3 * 86400
        await mem.store.update(item)
    await mem.remember("old idea", kind="note", scope="project", importance=0.1)
    old = (await mem.recall("old idea"))[0].item
    old.accessed_at = time.time() - 400 * 86400
    await mem.store.update(old)
    stats = await mem.consolidate(older_than_days=1, min_group=3)
    assert stats == {"summaries": 1, "archived": 4, "merged": 0}
    summary = (await mem.recall("", kinds=["note"], tags=["playtest"]))[0].item
    assert "Summary of 4 events" in summary.title and len(summary.links) == 4
    assert await mem.decay(half_life_days=30) >= 1
    assert all(h.item.text != "old idea" for h in await mem.recall("old idea"))
    assert await mem.recall("old idea", include_archived=True)


async def test_memory_tools_follow_the_callers_identity() -> None:
    mem = Memory(SQLiteMemoryStore())
    tools = {t.name: t for t in memory_tools(mem)}
    r = await tools["memory_remember"].run(
        {"text": "I hide keys under rocks", "scope": "agent"}, ToolContext(agent_id="mira")
    )
    assert not r.is_error and r.data["owner"] == "mira"
    mine = await tools["memory_recall"].run({"query": "keys rocks"}, ToolContext(agent_id="mira"))
    theirs = await tools["memory_recall"].run({"query": "keys rocks"}, ToolContext(agent_id="ash"))
    assert mine.data["hits"] and not theirs.data["hits"]
    anon = await tools["memory_remember"].run({"text": "x", "scope": "agent"}, ToolContext())
    assert anon.is_error and anon.error_code == "invalid_arguments"
    s = await tools["memory_summarize"].run({"query": "keys"}, ToolContext(agent_id="mira"))
    assert "keys" in s.text
    gone = await tools["memory_forget"].run({"query": "keys rocks"}, ToolContext(agent_id="mira"))
    assert gone.data["forgotten"] == 1


def test_fts_query_is_safe() -> None:
    assert fts_query('drop "table"; -- * OR') == '"drop" OR "table" OR "or"*'
    assert fts_query("!!!") == ""


async def test_store_list_filters() -> None:
    store = SQLiteMemoryStore()
    mem = Memory(store, agent="a", teams=["t"])
    await mem.remember("x1", kind="fact", scope="project", tags=["k"])
    await mem.remember("x2", kind="note", scope="team")
    items = await store.list_items(MemoryQuery(kinds=["fact"], tags=["k"]))
    assert [i.text for i in items] == ["x1"]
