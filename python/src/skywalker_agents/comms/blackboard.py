"""A blackboard on the studio board: agents post, claim, update and finish tasks everyone can see."""

from __future__ import annotations

from collections.abc import AsyncIterator, Sequence
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:  # pragma: no cover
    from ..engine.client import AsyncEngine


class Blackboard:
    """The studio board (``studio/board.json``), attributed per agent; the editor shows it live."""

    def __init__(self, engine: AsyncEngine) -> None:
        self.engine = engine

    def _as(self, agent: str | None) -> Any:
        return self.engine.as_agent(agent) if agent else self.engine

    async def post(
        self,
        title: str,
        *,
        description: str = "",
        acceptance: Sequence[str] = (),
        discipline: str = "",
        assignee: str = "",
        priority: str = "normal",
        feedback: Sequence[str] = (),
        by: str | None = None,
    ) -> dict[str, Any]:
        args: dict[str, Any] = {"title": title, "priority": priority}
        if description:
            args["description"] = description
        if acceptance:
            args["acceptance"] = list(acceptance)
        if discipline:
            args["discipline"] = discipline
        if assignee:
            args["assignee"] = assignee
        if feedback:
            args["feedback"] = list(feedback)
        res = await self._as(by).call("studio_task_create", args, check=True)
        return dict(res.data)

    async def claim(self, agent: str, task: str | None = None) -> dict[str, Any] | None:
        """Claims ``task`` (or the agent's next one). None when there is nothing to claim."""
        res = await self.engine.as_agent(agent).call("studio_task_claim", {"task": task} if task else {})
        if res.is_error:
            if res.error_code == "not_found":
                return None
            res.raise_for_error()
        return res.data

    async def update(
        self,
        task: str,
        *,
        by: str | None = None,
        status: str | None = None,
        comment: str | None = None,
        assignee: str | None = None,
    ) -> dict[str, Any]:
        args: dict[str, Any] = {"task": task}
        for k, v in (("status", status), ("comment", comment), ("assignee", assignee)):
            if v is not None:
                args[k] = v
        res = await self._as(by).call("studio_task_update", args, check=True)
        return dict(res.data)

    async def complete(self, task: str, *, by: str | None = None, report: str = "") -> dict[str, Any]:
        return await self.update(task, by=by, status="done", comment=report or None)

    async def tasks(self, status: str = "open", *, assignee: str | None = None) -> list[dict[str, Any]]:
        args: dict[str, Any] = {"status": status}
        if assignee:
            args["assignee"] = assignee
        res = await self.engine.call("studio_task_list", args, check=True)
        return list(res.data.get("tasks", []))

    async def get(self, task: str) -> dict[str, Any] | None:
        res = await self.engine.call("studio_task_list", {"limit": 1000}, check=True)
        return next((t for t in res.data.get("tasks", []) if t.get("id") == task), None)

    async def watch(self) -> AsyncIterator[dict[str, Any]]:
        """Task events as they happen (created, claimed, updated)."""
        async with self.engine.events(types=["studio.task"]) as stream:
            async for ev in stream:
                yield ev.model_dump()
