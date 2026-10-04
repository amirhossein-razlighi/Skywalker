"""Terminal rendering of JSONL traces (``sky-agents trace show|stats``)."""

from __future__ import annotations

from collections import defaultdict
from typing import Any

from . import tracing as tr


def _fmt_dur(s: float) -> str:
    return f"{s * 1000:.0f}ms" if s < 1 else f"{s:.1f}s"


def _line(span: dict[str, Any], verbose: bool) -> str:
    a = span.get("attributes") or {}
    parts = [span["name"], _fmt_dur(float(span.get("duration") or 0))]
    if span.get("kind") == "llm":
        parts.append(f"{a.get(tr.INPUT_TOKENS, 0)}→{a.get(tr.OUTPUT_TOKENS, 0)} tok")
        if a.get(tr.CACHE_READ_TOKENS):
            parts.append(f"cache {a.get(tr.CACHE_READ_TOKENS)}")
        reasons = a.get(tr.FINISH_REASONS)
        if reasons:
            parts.append(str(reasons[0] if isinstance(reasons, list) else reasons))
        calls = a.get("skywalker.tool_calls")
        if calls:
            parts.append("→ " + ", ".join(calls))
    if a.get(tr.COST):
        parts.append(f"${float(a[tr.COST]):.4f}")
    if span.get("kind") == "agent" and a.get("skywalker.stop_reason"):
        parts.append(str(a["skywalker.stop_reason"]))
    if span.get("status") == "error":
        parts.append(f"ERROR {span.get('error', '')[:120]}")
    out = "  ".join(parts)
    if verbose:
        for key in (
            "skywalker.task",
            "skywalker.tool.args",
            "skywalker.tool.result",
            "skywalker.completion",
            "skywalker.result",
        ):
            if a.get(key):
                text = str(a[key]).replace("\n", " ")
                out += f"\n      {key.split('.')[-1]}: {text[:240]}"
    return out


def render_tree(spans: list[dict[str, Any]], *, verbose: bool = False, max_depth: int = 12) -> str:
    """Spans as an indented tree (parents before children, in start order)."""
    if not spans:
        return "(empty trace)"
    by_parent: dict[str | None, list[dict[str, Any]]] = defaultdict(list)
    ids = {s["span_id"] for s in spans}
    for s in sorted(spans, key=lambda s: s.get("start") or 0):
        parent = s.get("parent_id")
        by_parent[parent if parent in ids else None].append(s)
    lines: list[str] = []

    def walk(parent: str | None, depth: int) -> None:
        for s in by_parent.get(parent, []):
            if depth <= max_depth:
                lines.append("  " * depth + ("└ " if depth else "") + _line(s, verbose))
            walk(s["span_id"], depth + 1)

    walk(None, 0)
    return "\n".join(lines)


def render_stats(spans: list[dict[str, Any]]) -> str:
    """Totals per agent and per tool: calls, tokens, cost, errors, time."""
    agents: dict[str, dict[str, float]] = defaultdict(lambda: defaultdict(float))
    tools: dict[str, dict[str, float]] = defaultdict(lambda: defaultdict(float))
    for s in spans:
        a = s.get("attributes") or {}
        agent = str(a.get(tr.AGENT_ID, "?"))
        if s.get("kind") == "llm":
            st = agents[agent]
            st["requests"] += 1
            st["input"] += float(a.get(tr.INPUT_TOKENS, 0) or 0)
            st["output"] += float(a.get(tr.OUTPUT_TOKENS, 0) or 0)
            st["cache_read"] += float(a.get(tr.CACHE_READ_TOKENS, 0) or 0)
            st["cost"] += float(a.get(tr.COST, 0) or 0)
            st["seconds"] += float(s.get("duration") or 0)
        elif s.get("kind") == "tool":
            t = tools[str(a.get(tr.TOOL_NAME, s["name"]))]
            t["calls"] += 1
            t["errors"] += 1 if s.get("status") == "error" else 0
            t["seconds"] += float(s.get("duration") or 0)
    lines = ["agents:"]
    total = 0.0
    for name, st in sorted(agents.items()):
        total += st["cost"]
        lines.append(
            f"  {name:20} {int(st['requests']):4} req  {int(st['input']):>9} in  {int(st['output']):>8} out  "
            f"{int(st['cache_read']):>9} cached  ${st['cost']:.4f}  {st['seconds']:.1f}s"
        )
    lines.append(f"  total ${total:.4f}")
    lines.append("tools:")
    for name, t in sorted(tools.items(), key=lambda kv: -kv[1]["calls"]):
        lines.append(f"  {name:28} {int(t['calls']):4} calls  {int(t['errors']):3} errors  {t['seconds']:.1f}s")
    return "\n".join(lines)
