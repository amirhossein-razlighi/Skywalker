"""A2A-style agent cards, so external agents can discover the studio's roles.

``sky-agents cards --project DIR --out cards/`` writes one ``<id>.json`` per roster member (and an
``index.json``) following the Agent2Agent AgentCard shape: name, description, url, version,
capabilities, input/output modes and skills, plus a ``skywalker`` block (role, discipline, focus,
tools). Serve them at ``/.well-known/agent-card.json`` behind your own A2A endpoint, or just use
them as a machine-readable roster.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .._version import __version__

A2A_PROTOCOL_VERSION = "0.3.0"


def agent_card(
    profile: dict[str, Any],
    *,
    base_url: str = "",
    tools: list[str] | None = None,
    organization: str = "Skywalker studio",
) -> dict[str, Any]:
    aid = str(profile.get("id", ""))
    role = str(profile.get("role", ""))
    focus = str(profile.get("focus", ""))
    name = str(profile.get("name") or aid)
    description = str(
        profile.get("mission")
        or f"{name}, the studio's {role.replace('_', ' ')}" + (f", focused on {focus}" if focus else "") + "."
    )
    tags = [t for t in [role, str(profile.get("discipline", "")), *profile.get("focus_tags", [])] if t]
    skills = [
        {
            "id": f"{aid}.{role or 'work'}",
            "name": role.replace("_", " ").title() or "Studio work",
            "description": description,
            "tags": tags,
            "examples": [f"@{aid} {focus or 'take the next task on the board'}"],
            "inputModes": ["text/plain", "application/json"],
            "outputModes": ["text/plain", "application/json", "image/png"],
        }
    ]
    url = f"{base_url.rstrip('/')}/agents/{aid}" if base_url else f"urn:skywalker:studio:agent:{aid}"
    return {
        "protocolVersion": A2A_PROTOCOL_VERSION,
        "name": name,
        "description": description,
        "url": url,
        "preferredTransport": "JSONRPC",
        "version": __version__,
        "provider": {"organization": organization, "url": base_url or "urn:skywalker"},
        "capabilities": {"streaming": False, "pushNotifications": False, "stateTransitionHistory": True},
        "defaultInputModes": ["text/plain", "application/json"],
        "defaultOutputModes": ["text/plain", "application/json"],
        "skills": skills,
        "skywalker": {
            "id": aid,
            "role": role,
            "discipline": profile.get("discipline", ""),
            "focus": focus,
            "autonomy": profile.get("autonomy", ""),
            "reports_to": profile.get("reports_to", ""),
            "model": profile.get("model", ""),
            "tools": tools or [],
        },
    }


def load_profiles(project: str | Path) -> list[dict[str, Any]]:
    """Roster profiles straight from ``agents/*.agent.json`` (no engine needed)."""
    out = []
    for p in sorted((Path(project) / "agents").glob("*.agent.json")):
        try:
            data = json.loads(p.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        data.setdefault("id", p.name[: -len(".agent.json")])
        out.append(data)
    return out


def export_cards(profiles: list[dict[str, Any]], out_dir: str | Path, *, base_url: str = "") -> list[Path]:
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    written = []
    index = []
    for prof in profiles:
        card = agent_card(prof, base_url=base_url)
        path = out / f"{prof['id']}.json"
        path.write_text(json.dumps(card, indent=2) + "\n")
        written.append(path)
        index.append({"id": prof["id"], "name": card["name"], "url": card["url"], "card": path.name})
    (out / "index.json").write_text(json.dumps({"agents": index}, indent=2) + "\n")
    return written
