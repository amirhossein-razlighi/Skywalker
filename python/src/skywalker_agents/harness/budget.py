"""Budgets: tokens, cost, time, tool calls and turns, enforced per agent, per step and per run.

Trackers nest: an agent's tracker charges its workflow's tracker, so a run-wide limit holds even
when many agents work in parallel.
"""

from __future__ import annotations

import threading
import time
from typing import Any

from pydantic import BaseModel

from ..errors import BudgetExceeded
from ..llm.types import Usage


class Budget(BaseModel):
    max_tokens: int | None = None
    max_cost_usd: float | None = None
    max_seconds: float | None = None
    max_tool_calls: int | None = None
    max_turns: int | None = None

    def is_unlimited(self) -> bool:
        return all(v is None for v in self.model_dump().values())


class BudgetTracker:
    def __init__(
        self, budget: Budget | None = None, *, scope: str = "run", parent: BudgetTracker | None = None
    ) -> None:
        self.budget = budget or Budget()
        self.scope = scope
        self.parent = parent
        self.tokens = 0
        self.cost_usd = 0.0
        self.tool_calls = 0
        self.turns = 0
        self.started = time.monotonic()
        self._lock = threading.Lock()

    def child(self, budget: Budget | None = None, scope: str = "") -> BudgetTracker:
        return BudgetTracker(budget, scope=scope or self.scope, parent=self)

    @property
    def elapsed(self) -> float:
        return time.monotonic() - self.started

    def check(self) -> None:
        """Raises :class:`BudgetExceeded` if this tracker (or an ancestor) is over a limit."""
        b = self.budget
        if b.max_tokens is not None and self.tokens > b.max_tokens:
            raise BudgetExceeded(self.scope, "tokens", self.tokens, b.max_tokens)
        if b.max_cost_usd is not None and self.cost_usd > b.max_cost_usd:
            raise BudgetExceeded(self.scope, "cost_usd", self.cost_usd, b.max_cost_usd)
        if b.max_seconds is not None and self.elapsed > b.max_seconds:
            raise BudgetExceeded(self.scope, "seconds", self.elapsed, b.max_seconds)
        if b.max_tool_calls is not None and self.tool_calls > b.max_tool_calls:
            raise BudgetExceeded(self.scope, "tool_calls", self.tool_calls, b.max_tool_calls)
        if b.max_turns is not None and self.turns > b.max_turns:
            raise BudgetExceeded(self.scope, "turns", self.turns, b.max_turns)
        if self.parent is not None:
            self.parent.check()

    def charge_llm(self, usage: Usage, cost_usd: float) -> None:
        with self._lock:
            self.tokens += usage.total_tokens
            self.cost_usd += cost_usd
        if self.parent is not None:
            self.parent.charge_llm(usage, cost_usd)

    def charge_tool(self, n: int = 1) -> None:
        with self._lock:
            self.tool_calls += n
        if self.parent is not None:
            self.parent.charge_tool(n)

    def charge_turn(self) -> None:
        with self._lock:
            self.turns += 1
        if self.parent is not None:
            self.parent.charge_turn()

    def would_exceed_tools(self) -> bool:
        b = self.budget
        over = b.max_tool_calls is not None and self.tool_calls >= b.max_tool_calls
        return over or (self.parent is not None and self.parent.would_exceed_tools())

    def snapshot(self) -> dict[str, Any]:
        return {
            "scope": self.scope,
            "tokens": self.tokens,
            "cost_usd": round(self.cost_usd, 6),
            "tool_calls": self.tool_calls,
            "turns": self.turns,
            "seconds": round(self.elapsed, 3),
            "limits": self.budget.model_dump(exclude_none=True),
        }
