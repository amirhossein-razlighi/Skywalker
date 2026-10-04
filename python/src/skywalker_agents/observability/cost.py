"""Cost accounting per agent and model."""

from __future__ import annotations

import threading
from dataclasses import dataclass, field
from typing import Any

from ..llm.pricing import cost_usd, price_for
from ..llm.types import Usage


@dataclass
class CostLine:
    usage: Usage = field(default_factory=Usage)
    cost_usd: float = 0.0
    unpriced: bool = False


class CostMeter:
    """Accumulates tokens and estimated USD by (agent, model). Thread-safe."""

    def __init__(self) -> None:
        self._lines: dict[tuple[str, str], CostLine] = {}
        self._lock = threading.Lock()

    def add(self, agent: str, model: str, usage: Usage) -> float:
        cost = cost_usd(model, usage)
        with self._lock:
            line = self._lines.setdefault((agent, model), CostLine())
            line.usage = line.usage + usage
            line.cost_usd += cost
            line.unpriced = price_for(model) is None
        return cost

    @property
    def total_usd(self) -> float:
        with self._lock:
            return sum(x.cost_usd for x in self._lines.values())

    @property
    def total_usage(self) -> Usage:
        with self._lock:
            total = Usage()
            for x in self._lines.values():
                total = total + x.usage
            return total

    def by_agent(self) -> dict[str, dict[str, Any]]:
        out: dict[str, dict[str, Any]] = {}
        with self._lock:
            for (agent, model), line in self._lines.items():
                a = out.setdefault(agent, {"cost_usd": 0.0, "tokens": 0, "requests": 0, "models": {}})
                a["cost_usd"] += line.cost_usd
                a["tokens"] += line.usage.total_tokens
                a["requests"] += line.usage.requests
                a["models"][model] = {
                    **line.usage.model_dump(),
                    "cost_usd": round(line.cost_usd, 6),
                    "unpriced": line.unpriced,
                }
        return out

    def report(self) -> dict[str, Any]:
        return {
            "total_usd": round(self.total_usd, 6),
            "usage": self.total_usage.model_dump(),
            "agents": self.by_agent(),
        }
