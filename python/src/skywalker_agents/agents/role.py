"""Roles: who an agent is. Read from the project's roster (``agents/<id>.agent.json``) and, when an engine
is available, from ``studio_agent_brief`` (the engine's own system prompt and permitted tools), so a
Python agent runs exactly like the editor's crew member of the same id."""

from __future__ import annotations

import json
from pathlib import Path
from typing import TYPE_CHECKING, Any, Literal

from pydantic import BaseModel, ConfigDict, Field

from ..llm import CHEAP_MODEL, DEFAULT_MODEL

if TYPE_CHECKING:  # pragma: no cover
    from ..engine.client import AsyncEngine

# Roles whose work is high-volume and well specified: default to the cheaper model.
CHEAP_ROLES = frozenset({"playtester", "qa_lead"})


class Role(BaseModel):
    model_config = ConfigDict(extra="allow", populate_by_name=True)

    id: str
    name: str = ""
    role: str = ""
    discipline: str = ""
    focus: str = ""
    focus_tags: list[str] = Field(default_factory=list)
    persona: str = ""
    mission: str = ""
    instructions: str = ""
    provider: str = "anthropic"
    model: str = ""
    autonomy: Literal["observe", "ask", "autonomous"] = "autonomous"
    permissions: dict[str, str] = Field(default_factory=dict)
    reports_to: str = ""
    memory: list[str] = Field(default_factory=list)
    max_rounds: int = Field(default=40, alias="max_rounds")
    teams: list[str] = Field(default_factory=list)
    # From studio_agent_brief (empty when the role was built offline).
    system_prompt: str = ""
    brief_tools: list[dict[str, Any]] = Field(default_factory=list)
    on_roster: bool = False

    @property
    def default_model(self) -> str:
        if self.model:
            return self.model
        if self.provider not in ("anthropic", ""):
            return ""
        return CHEAP_MODEL if self.role in CHEAP_ROLES else DEFAULT_MODEL

    @property
    def display(self) -> str:
        return self.name or self.id

    @classmethod
    def load(cls, project: str | Path, agent_id: str) -> Role:
        """From ``agents/<id>.agent.json`` (no engine needed)."""
        path = Path(project) / "agents" / f"{agent_id}.agent.json"
        if not path.exists():
            known = sorted(p.name[: -len(".agent.json")] for p in (Path(project) / "agents").glob("*.agent.json"))
            raise FileNotFoundError(f"no agent '{agent_id}' in {project}/agents (known: {', '.join(known) or 'none'})")
        data = json.loads(path.read_text())
        data.setdefault("id", agent_id)
        data["on_roster"] = True
        return cls.model_validate(_clean(data))

    @classmethod
    async def from_engine(cls, engine: AsyncEngine, agent_id: str, *, loop_member: bool = False) -> Role:
        """The roster member as the engine sees it: profile, system prompt and permitted tools."""
        brief = await engine.call("studio_agent_brief", {"agent": agent_id, "loop_member": loop_member}, check=True)
        roster = await engine.call("studio_agent_list", {"include_profiles": True})
        profile: dict[str, Any] = {}
        for a in roster.data.get("agents", []):
            if a.get("id") == brief.data.get("agent"):
                profile = dict(a.get("profile") or a)
        data = {**_clean(profile), "id": brief.data.get("agent", agent_id)}
        data["system_prompt"] = str(brief.data.get("system_prompt", ""))
        data["brief_tools"] = list(brief.data.get("tools", []))
        if brief.data.get("model"):
            data["model"] = brief.data["model"]
        if brief.data.get("provider"):
            data["provider"] = brief.data["provider"]
        if brief.data.get("max_rounds"):
            data["max_rounds"] = brief.data["max_rounds"]
        data["on_roster"] = True
        return cls.model_validate(data)

    @classmethod
    def adhoc(
        cls,
        agent_id: str,
        *,
        role: str = "",
        instructions: str = "",
        model: str = "",
        provider: str = "anthropic",
        **extra: Any,
    ) -> Role:
        """A role that is not on the studio roster (scripts, evaluators, judges)."""
        return cls(
            id=agent_id,
            name=extra.pop("name", agent_id),
            role=role,
            instructions=instructions,
            model=model,
            provider=provider,
            **extra,
        )

    def local_system_prompt(self) -> str:
        """The system prompt when no engine brief is available."""
        if self.system_prompt:
            return self.system_prompt
        parts = [
            f"You are {self.display} (@{self.id})"
            + (f", the studio's {self.role.replace('_', ' ')}" if self.role else "")
            + "."
        ]
        if self.focus:
            parts.append(f"Your focus: {self.focus}.")
        if self.mission:
            parts.append(self.mission)
        if self.persona:
            parts.append(f"Persona: {self.persona}")
        if self.instructions:
            parts.append(f"Standing instructions: {self.instructions}")
        if self.memory:
            parts.append("Your notes:\n" + "\n".join(f"- {m}" for m in self.memory))
        parts.append(
            "Work in small verified steps: act with tools, look at the result, then report briefly what "
            "you did and what is left."
        )
        return "\n\n".join(parts)

    def ask_tools(self) -> list[str]:
        return [t["name"] for t in self.brief_tools if t.get("access") == "ask"]

    def permitted_tools(self) -> list[str]:
        return [t["name"] for t in self.brief_tools if t.get("access") in ("allow", "ask")]


def _clean(data: dict[str, Any]) -> dict[str, Any]:
    out = {k: v for k, v in data.items() if k not in ("format", "version", "status", "usage", "open_tasks")}
    if isinstance(out.get("permissions"), dict):
        out["permissions"] = {str(k): str(v) for k, v in out["permissions"].items()}
    if out.get("autonomy") not in (None, "observe", "ask", "autonomous"):
        out.pop("autonomy")
    return out
