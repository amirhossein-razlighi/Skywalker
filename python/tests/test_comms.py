from __future__ import annotations

import asyncio
import json
from pathlib import Path

from skywalker_agents import AsyncEngine, Blackboard, FakeEngine, LocalBus, StudioBus
from skywalker_agents.comms import HANDOFF, Envelope, agent_card, collect, export_cards, load_profiles


async def test_local_bus_direct_topics_threads_and_request_reply(tmp_path: Path) -> None:
    bus = LocalBus(tmp_path / "bus.jsonl")
    inbox = bus.subscribe(agent="stratus")
    art = bus.subscribe(topic="art")
    await bus.tell("nimbus", "stratus", "widen the bridge", data={"width": 3})
    await bus.publish("aurora", "art", "new palette")
    got = await collect(inbox, 1, timeout=1)
    assert got[0].data == {"width": 3} and got[0].kind == "inform"
    assert (await collect(art, 1, timeout=1))[0].text == "new palette"

    async def responder() -> None:
        env = await inbox.get(2)
        assert env is not None
        await bus.reply(env, "stratus", "done", data={"width": 3})

    task = asyncio.create_task(responder())
    reply = await bus.request("nimbus", "stratus", "status?", timeout=2)
    await task
    assert reply is not None and reply.text == "done" and reply.kind == "result"
    thread = await bus.thread(reply.thread)
    assert [m.text for m in thread] == ["status?", "done"]
    assert len((await bus.inbox("stratus"))) == 2 and await bus.inbox("stratus") == []
    reloaded = LocalBus(tmp_path / "bus.jsonl")
    assert len(reloaded.messages) == len(bus.messages)
    assert await bus.request("nimbus", "aurora", "anyone?", timeout=0.1) is None


async def test_studio_bus_rides_on_engine_messages(engine: AsyncEngine, fake: FakeEngine) -> None:
    bus = StudioBus(engine)
    sub = bus.subscribe(agent="stratus")
    await bus.ready()
    sent = await bus.tell("nimbus", "stratus", "@stratus fix T-1", kind="request", data={"task": "T-1"})
    assert sent.sender == "nimbus" and sent.id.startswith("M-")
    # The message is a studio message: persisted and attributed in the engine.
    assert fake.messages[-1]["from"] == "nimbus" and fake.messages[-1]["kind"] == "request"
    got = await sub.get(3)
    assert got is not None and got.data == {"task": "T-1"}

    async def responder() -> None:
        env = await sub.get(3)
        assert env is not None
        await bus.reply(env, "stratus", "on it")

    task = asyncio.create_task(responder())
    reply = await bus.request("nimbus", "stratus", "ETA?", timeout=3)
    await task
    assert reply is not None and reply.text == "on it"
    assert [m.text for m in await bus.thread(reply.root)] == ["ETA?", "on it"]
    board = Blackboard(engine)
    handoff = await bus.handoff("nimbus", "aurora", "Light the canyon", context={"mood": "warm"}, board=board,
                                acceptance=["sunset reads warm"])
    assert handoff.kind == HANDOFF and handoff.refs["task"] == "T-1"
    task_ = (await board.tasks(assignee="aurora"))[0]
    assert task_["title"] == "Light the canyon"
    claimed = await board.claim("aurora")
    assert claimed is not None and claimed["status"] == "doing"
    done = await board.complete(claimed["id"], by="aurora", report="lit")
    assert done["status"] == "done" and await board.claim("aurora") is None
    await bus.close()


def test_envelope_round_trip() -> None:
    env = Envelope(sender="a", to=["b"], text="hi", kind="request", data={"x": 1}, reply_to="M-1", topic="art",
                   refs={"task": "T-2"})
    args = env.to_studio_args()
    assert args == {"text": "hi", "to": ["b"], "channel": "art", "reply_to": "M-1", "kind": "request",
                    "data": {"x": 1}, "task": "T-2"}
    back = Envelope.from_studio({"id": "M-2", "from": "a", "text": "hi", "thread": "M-1", "kind": "result"})
    assert back.root == "M-1" and back.kind == "result"


def test_agent_cards(project: Path) -> None:
    (project / "agents" / "mira.agent.json").write_text(json.dumps(
        {"name": "Mira", "role": "level_designer", "discipline": "design", "focus": "secret areas",
         "focus_tags": ["secrets"]}))
    profiles = load_profiles(project)
    card = agent_card(profiles[0], base_url="https://studio.example")
    assert card["url"] == "https://studio.example/agents/mira" and card["skills"][0]["tags"][-1] == "secrets"
    written = export_cards(profiles, project / "cards")
    assert written[0].name == "mira.json"
    assert json.loads((project / "cards" / "index.json").read_text())["agents"][0]["id"] == "mira"
