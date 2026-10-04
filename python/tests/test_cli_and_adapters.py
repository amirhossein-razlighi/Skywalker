from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

from skywalker_agents import Memory, Tracer
from skywalker_agents.cli.main import main
from skywalker_agents.memory import SQLiteMemoryStore
from skywalker_agents.observability.tracing import JsonlSink
from skywalker_agents.plugins.registry import PluginRegistry
from skywalker_agents.plugins.template import scaffold_plugin
from skywalker_agents.tools import Tool, ToolContext, tool


def test_cli_memory_round_trip(project: Path, capsys: pytest.CaptureFixture[str]) -> None:
    assert main(["memory", "add", "Bridges", "stay", "3", "m", "wide", "--project", str(project), "--kind", "fact",
                 "--scope", "project", "--tags", "level"]) == 0
    assert main(["memory", "search", "bridge", "width", "--project", str(project), "--json"]) == 0
    out = capsys.readouterr().out
    hits = json.loads(out[out.index("["):])
    assert hits[0]["text"] == "Bridges stay 3 m wide"
    assert main(["memory", "export", "--project", str(project)]) == 0
    exported = capsys.readouterr().out.strip().splitlines()
    assert json.loads(exported[0])["kind"] == "fact"
    assert main(["memory", "stats", "--project", str(project)]) == 0
    assert '"total": 1' in capsys.readouterr().out


def test_cli_run_trace_and_cards(project: Path, tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    wf = tmp_path / "hello.yaml"
    wf.write_text("name: hello\ninputs: {who: world}\nsteps:\n"
                  "  - {id: look, tool: scene_overview}\n"
                  "  - {id: greet, agent: nimbus, after: [look], prompt: 'Say hi to {{ inputs.who }}'}\n")
    script = tmp_path / "script.json"
    script.write_text(json.dumps({"agents": {"nimbus": [{"text": "hi world"}]}}))
    code = main(["run", str(wf), "--fake", "--project", str(project), "--script", str(script), "--no-memory",
                 "--input", "who=everyone"])
    out = capsys.readouterr().out
    assert code == 0, out
    assert "completed" in out and "hi world" in out
    assert main(["trace", "list", "--project", str(project)]) == 0
    assert "hello" in capsys.readouterr().out
    assert main(["trace", "show", "latest", "--project", str(project), "-v"]) == 0
    tree = capsys.readouterr().out
    assert "workflow hello" in tree and "invoke_agent nimbus" in tree
    assert main(["trace", "stats", "latest", "--project", str(project)]) == 0
    (project / "agents" / "nimbus.agent.json").write_text(json.dumps({"name": "Nimbus", "role": "creative_director"}))
    assert main(["cards", "--project", str(project), "--out", str(tmp_path / "cards")]) == 0
    assert (tmp_path / "cards" / "nimbus.json").exists()
    assert main(["doctor"]) == 0


def test_plugin_scaffold_registers(tmp_path: Path) -> None:
    root = scaffold_plugin("Hazard Tools", tmp_path)
    assert (root / "pyproject.toml").read_text().count("skywalker_agents.plugins") == 1
    sys.path.insert(0, str(root / "src"))
    try:
        import hazard_tools  # type: ignore[import-not-found]

        reg = PluginRegistry()
        hazard_tools.register(reg)
        assert "hazard_tools_hazard_count" in reg.tools and "hazard_tools_two_pass" in reg.patterns
        assert isinstance(reg.tools.get("hazard_tools_hazard_count"), Tool)
    finally:
        sys.path.remove(str(root / "src"))
    with pytest.raises(FileExistsError):
        scaffold_plugin("Hazard Tools", tmp_path)
    reg = PluginRegistry()
    reg.tools.add("hazards", 1)
    with pytest.raises(Exception, match="already registered"):
        reg.tools.add("hazards", 2)
    with pytest.raises(Exception, match="did you mean 'hazards'"):
        reg.tools.get("hazard")


@tool
async def double(n: int) -> dict[str, Any]:
    """Double a number.

    Args:
        n: The number.
    """
    return {"value": n * 2}


async def test_langchain_adapter() -> None:
    pytest.importorskip("langchain_core")
    from skywalker_agents.adapters.langchain import as_langchain_tools

    lc = as_langchain_tools([double])[0]
    assert lc.name == "double"
    assert json.loads(await lc.ainvoke({"n": 4})) == {"value": 8}


async def test_other_framework_adapters() -> None:
    try:
        from skywalker_agents.adapters.openai_agents import as_openai_agents_tools

        ft = as_openai_agents_tools([double])[0]
        assert ft.name == "double" and json.loads(await ft.on_invoke_tool(None, '{"n": 2}')) == {"value": 4}
    except ImportError:
        pass
    try:
        from skywalker_agents.adapters.pydantic_ai import as_pydantic_ai_tools

        assert as_pydantic_ai_tools([double])[0].name == "double"
    except ImportError:
        pass
    try:
        from skywalker_agents.adapters.claude_agent_sdk import as_sdk_mcp_server

        server = as_sdk_mcp_server([double])
        assert server["name"] == "sky-agents"
    except ImportError:
        pass


async def test_mcp_server_lists_and_calls_memory_tools() -> None:
    pytest.importorskip("mcp")
    import mcp.types as types

    from skywalker_agents.adapters.mcp_server import build_server

    mem = Memory(SQLiteMemoryStore())
    server = build_server(mem.tools(), agent="claude-code")
    listed = await server.request_handlers[types.ListToolsRequest](types.ListToolsRequest(method="tools/list"))
    names = [t.name for t in listed.root.tools]
    assert names == ["memory_remember", "memory_recall", "memory_forget", "memory_summarize"]
    req = types.CallToolRequest(method="tools/call", params=types.CallToolRequestParams(
        name="memory_remember", arguments={"text": "Claude Code was here", "scope": "agent"}))
    res = (await server.request_handlers[types.CallToolRequest](req)).root
    assert not res.isError and res.structuredContent["owner"] == "claude-code"


def test_otel_export(tmp_path: Path) -> None:
    pytest.importorskip("opentelemetry.sdk")
    from opentelemetry.sdk.trace import TracerProvider
    from opentelemetry.sdk.trace.export import SimpleSpanProcessor
    from opentelemetry.sdk.trace.export.in_memory_span_exporter import InMemorySpanExporter

    from skywalker_agents.observability.otel import OTelSink

    exporter = InMemorySpanExporter()
    provider = TracerProvider()
    provider.add_span_processor(SimpleSpanProcessor(exporter))
    tracer = Tracer([OTelSink(provider), JsonlSink(tmp_path / "t.jsonl")])
    with tracer.span("invoke_agent mira", "agent", **{"gen_ai.agent.id": "mira"}), tracer.span(
            "chat claude-opus-5-5", "llm", **{"gen_ai.usage.input_tokens": 5, "skywalker.prompt": "secret"}):
        pass
    spans = {s.name: s for s in exporter.get_finished_spans()}
    chat = spans["chat claude-opus-5-5"]
    assert chat.attributes["gen_ai.operation.name"] == "chat"
    assert "skywalker.prompt" not in chat.attributes  # content stays out unless capture_content
    assert chat.parent is not None and chat.parent.span_id == spans["invoke_agent mira"].context.span_id
    assert len((tmp_path / "t.jsonl").read_text().splitlines()) == 2


def test_cli_entry_point_installed() -> None:
    out = subprocess.run([sys.executable, "-m", "skywalker_agents.cli.main", "--version"], capture_output=True,
                         text=True, check=True)
    assert out.stdout.startswith("sky-agents")


def test_tool_context_defaults() -> None:
    assert ToolContext().agent_id == ""
