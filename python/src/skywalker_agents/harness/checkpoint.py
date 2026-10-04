"""Run directories and checkpoints (``<project>/studio/runs/<run id>/``)."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

from pydantic import BaseModel, Field


def runs_root(project: str | os.PathLike[str] | None) -> Path:
    """Where runs live: the project's ``studio/runs`` or the per-user state dir."""
    if project:
        return Path(project) / "studio" / "runs"
    from ..memory.memory import home

    return home() / "runs"


def run_dir_for(project: str | os.PathLike[str] | None, run_id: str) -> Path:
    return runs_root(project) / run_id


class Checkpoint(BaseModel):
    run_id: str
    workflow: str
    status: str = "running"
    iteration: int = 1
    inputs: dict[str, Any] = Field(default_factory=dict)
    steps: dict[str, Any] = Field(default_factory=dict)  # this iteration's step records
    history: list[dict[str, Any]] = Field(default_factory=list)
    state: dict[str, Any] = Field(default_factory=dict)
    spent: dict[str, Any] = Field(default_factory=dict)


class CheckpointStore:
    """Atomic JSON checkpoints in a run directory."""

    def __init__(self, run_dir: str | os.PathLike[str]) -> None:
        self.dir = Path(run_dir)
        self.path = self.dir / "checkpoint.json"

    def save(self, ckpt: Checkpoint) -> None:
        self.dir.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(ckpt.model_dump(mode="json"), indent=2, default=str))
        os.replace(tmp, self.path)

    def load(self) -> Checkpoint | None:
        if not self.path.exists():
            return None
        return Checkpoint.model_validate_json(self.path.read_text())


def list_runs(project: str | os.PathLike[str] | None) -> list[dict[str, Any]]:
    """Runs under a project (newest first) with their status."""
    root = runs_root(project)
    out: list[dict[str, Any]] = []
    if not root.exists():
        return out
    for d in sorted(root.iterdir(), key=lambda p: p.stat().st_mtime, reverse=True):
        if not d.is_dir():
            continue
        info: dict[str, Any] = {"run_id": d.name, "dir": str(d)}
        report = d / "report.json"
        ckpt = d / "checkpoint.json"
        try:
            if report.exists():
                r = json.loads(report.read_text())
                info.update({k: r.get(k) for k in ("workflow", "status", "iterations", "seconds")})
                info["cost_usd"] = (r.get("cost") or {}).get("total_usd")
            elif ckpt.exists():
                c = json.loads(ckpt.read_text())
                info.update(
                    {"workflow": c.get("workflow"), "status": c.get("status"), "iterations": c.get("iteration")}
                )
        except (OSError, json.JSONDecodeError):
            info["status"] = "unreadable"
        info["has_trace"] = (d / "trace.jsonl").exists()
        out.append(info)
    return out
