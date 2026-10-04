"""The shipped examples run end to end (scripted models) against a real headless engine."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
from pathlib import Path

import pytest

from skywalker_agents import AsyncEngine, ToolContext

pytestmark = pytest.mark.integration
EXAMPLES = Path(__file__).resolve().parents[2] / "examples"


def _run(script: Path, binary: str, *args: str) -> str:
    out = subprocess.run(
        [sys.executable, str(script), "--binary", binary, *args], capture_output=True, text=True, timeout=300
    )
    assert out.returncode == 0, out.stdout + out.stderr
    return out.stdout


def test_playtest_triage_fix_example(binary: str) -> None:
    out = _run(EXAMPLES / "playtest_triage_fix" / "run.py", binary, "--dry-run")
    assert "completed: completion 0 -> 1" in out


def test_level_design_critic_example(binary: str) -> None:
    out = _run(EXAMPLES / "level_design_critic" / "run.py", binary, "--dry-run")
    assert "approved=True" in out and "jump gaps must stay at most 4 m" in out


def test_langgraph_example(binary: str) -> None:
    if importlib.util.find_spec("langgraph") is None:
        pytest.skip("langgraph not installed")
    out = _run(EXAMPLES / "langgraph_adapter.py", binary, "--offline")
    assert "spikes every 14 m" in out


async def test_custom_plugin_tool_on_the_engine(binary: str, sky_dash: Path) -> None:
    sys.path.insert(0, str(EXAMPLES / "custom_plugin" / "src"))
    try:
        import sky_agents_hazards as plugin  # type: ignore[import-not-found]
    finally:
        sys.path.pop(0)
    engine = await AsyncEngine.spawn(sky_dash, binary=binary)
    try:
        res = await plugin.hazard_audit.run({"min_gap": 15}, ToolContext(engine=engine))
        assert res.data["count"] >= 8, res.text
        assert res.data["too_close"], "spikes and slimes alternate every 7 m: closer than 15 m"
        host = await engine.host_tools([plugin.hazard_audit], label="hazards plugin")
        served = await engine.as_agent("critic").call("py_hazard_audit", {"min_gap": 15}, check=True)
        assert served.data["count"] == res.data["count"]
        await host.stop()
    finally:
        await engine.close()
