"""``sky-agents``: run workflows, chat with roles, manage memory, read traces, serve tools.

sky-agents run workflows/lighting.yaml --project examples/sky_dash --input goal="warm sunset"
sky-agents chat mira --project examples/sky_dash
sky-agents memory search "bridge width" --project examples/sky_dash
sky-agents trace show latest --project examples/sky_dash
sky-agents serve-mcp --project examples/sky_dash          # shared memory for Claude Code / Codex
sky-agents host-tools --project examples/sky_dash         # py_memory_* tools on the running engine
sky-agents new plugin my_tools
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
from pathlib import Path
from typing import Any

from .._version import __version__


def _add_engine_args(p: argparse.ArgumentParser) -> None:
    g = p.add_argument_group("engine")
    g.add_argument("--project", default=os.environ.get("SKY_PROJECT", "."), help="game project directory")
    g.add_argument(
        "--attach",
        nargs="?",
        const="",
        default=None,
        metavar="SOCKET",
        help="attach to the running editor (or a `skywalker serve` socket)",
    )
    g.add_argument("--spawn", action="store_true", help="start a headless engine (skywalker serve) on --project")
    g.add_argument("--stdio", action="store_true", help="with --spawn: use `skywalker mcp` on stdio")
    g.add_argument("--fake", action="store_true", help="use the in-process fake engine (offline)")
    g.add_argument("--binary", default=None, help="path to the skywalker CLI (default: $SKYWALKER_BIN or PATH)")


def _add_model_args(p: argparse.ArgumentParser) -> None:
    g = p.add_argument_group("model")
    g.add_argument("--provider", default=None, help="anthropic (default), openai, ollama, lmstudio, vllm, litellm, ...")
    g.add_argument("--model", default=None, help="model id (default: the role's, else claude-opus-5-5)")
    g.add_argument("--script", default=None, help="scripted model replies (JSON, `studio run --mock` format)")
    g.add_argument("--dry-run", action="store_true", help="no model calls: a stub model that ends every turn")


async def _engine(args: argparse.Namespace) -> Any:
    from ..engine.client import AsyncEngine

    if args.fake:
        return AsyncEngine.fake()
    if args.attach is not None:
        return await AsyncEngine.attach(args.attach or None, project=args.project)
    if args.spawn:
        return await AsyncEngine.spawn(args.project, binary=args.binary, transport="stdio" if args.stdio else "serve")
    return await AsyncEngine.auto(args.project, binary=args.binary)


def _provider(args: argparse.Namespace) -> Any:
    from ..llm import ScriptedProvider, get_provider

    if getattr(args, "script", None):
        return ScriptedProvider.from_file(args.script)
    if getattr(args, "dry_run", False):
        return ScriptedProvider(exhausted="(dry run: no model was called)")
    return get_provider(args.provider) if args.provider else None


def _approver(name: str, engine: Any) -> Any:
    from ..harness import approvals

    if name == "studio":
        return approvals.StudioApprover(engine)
    return {"auto": approvals.AutoApprove(), "deny": approvals.AutoDeny(), "terminal": approvals.TerminalApprover()}[
        name
    ]


def _print(obj: Any, as_json: bool) -> None:
    if as_json:
        print(json.dumps(obj, indent=2, default=str))
    else:
        print(obj if isinstance(obj, str) else json.dumps(obj, indent=2, default=str))


# ---------------------------------------------------------------------- run
async def cmd_run(args: argparse.Namespace) -> int:
    from ..harness.budget import Budget
    from ..harness.declarative import load_workflow
    from ..harness.replay import Recorder, Replayer
    from ..memory import Memory
    from ..observability.tracing import Tracer

    wf, spec = load_workflow(args.workflow)
    inputs = dict(spec.get("inputs") or {})
    for kv in args.input or []:
        k, _, v = kv.partition("=")
        inputs[k] = _coerce(v)
    engine = await _engine(args)
    middleware: list[Any] = []
    provider = _provider(args)
    if args.record:
        middleware.append(Recorder(args.record))
    if args.replay:
        rep = Replayer(args.replay)
        middleware.append(rep)
        provider = rep.provider()
    tracer = Tracer()
    if args.otel:
        from ..observability.otel import OTelSink, configure_otel

        tracer.add_sink(OTelSink(configure_otel(args.otel)))
    budget = None
    if args.max_cost is not None or args.max_minutes is not None:
        budget = Budget(max_cost_usd=args.max_cost, max_seconds=args.max_minutes * 60 if args.max_minutes else None)
    memory = None if args.no_memory or engine.mode == "fake" else Memory.open(args.project)
    try:
        approver = _approver(args.approve or ("studio" if engine.mode == "socket" else "deny"), engine)
        result = await wf.run(
            engine,
            inputs=inputs,
            provider=provider,
            memory=memory,
            approver=approver,
            middleware=middleware,
            tracer=tracer,
            resume=args.resume,
            project=args.project,
            budget=budget,
        )
    finally:
        await engine.close()
        if memory is not None:
            await memory.close()
    if args.json:
        _print(result.model_dump(mode="json"), True)
    else:
        print(
            f"run {result.run_id}: {result.status} after {result.iterations} iteration(s), "
            f"${result.cost.get('total_usd', 0):.4f}, {result.seconds:.1f}s"
        )
        if result.error:
            print(f"  error: {result.error}")
        for k, v in result.results.items():
            text = v.get("text") if isinstance(v, dict) else v
            print(f"  {k}: {str(text)[:300]}")
        print(f"  trace: sky-agents trace show {result.run_id} --project {args.project}")
    return 0 if result.ok else 1


def _coerce(v: str) -> Any:
    try:
        return json.loads(v)
    except json.JSONDecodeError:
        return v


# ---------------------------------------------------------------------- chat
async def cmd_chat(args: argparse.Namespace) -> int:
    from ..agents import Agent
    from ..harness.approvals import TerminalApprover
    from ..memory import Memory

    engine = await _engine(args)
    memory = Memory.open(args.project) if not args.no_memory and engine.mode != "fake" else None
    agent = Agent(
        args.role, engine=engine, provider=_provider(args), model=args.model, memory=memory, approver=TerminalApprover()
    )
    await agent.setup()
    print(f"chatting with @{agent.id} ({agent.model}); empty line or Ctrl-D to quit")
    try:
        while True:
            try:
                line = await asyncio.to_thread(input, "you> ")
            except EOFError:
                break
            if not line.strip():
                break
            res = await agent.chat(line)
            used = ", ".join(sorted({c.name for c in res.tool_calls}))
            print(f"@{agent.id}> {res.text or res.error}")
            print(
                f"   ({res.stop_reason}, {res.turns} turn(s), ${res.cost_usd:.4f}{', tools: ' + used if used else ''})"
            )
    finally:
        await engine.close()
        if memory is not None:
            await memory.close()
    return 0


# ---------------------------------------------------------------------- memory
async def cmd_memory(args: argparse.Namespace) -> int:
    from ..memory import HashingEmbedder, Memory

    embedder = HashingEmbedder() if args.semantic else None
    mem = Memory.open(
        args.project, agent=args.agent or "", teams=args.team or [], embedder=embedder, global_memory=not args.no_global
    )
    try:
        if args.action == "add":
            item = await mem.remember(
                " ".join(args.text),
                kind=args.kind or "note",
                scope=args.scope or ("agent" if args.agent else "project"),
                tags=args.tags or [],
                importance=args.importance,
            )
            _print(item.model_dump(mode="json") if args.json else f"remembered {item.id}", args.json)
        elif args.action in ("search", "recall"):
            hits = await mem.recall(
                " ".join(args.text),
                scopes=[args.scope] if args.scope else None,
                kinds=[args.kind] if args.kind else None,
                tags=args.tags,
                k=args.limit,
            )
            if args.json:
                _print([{"score": h.score, **h.item.model_dump(mode="json")} for h in hits], True)
            else:
                for h in hits:
                    it = h.item
                    print(
                        f"{h.score:5.2f}  {it.id}  {it.kind:8} {it.scope}{'/' + it.owner if it.owner else ''}  "
                        f"{it.short(110)}"
                    )
        elif args.action == "list":
            hits = await mem.recall(
                "",
                scopes=[args.scope] if args.scope else None,
                kinds=[args.kind] if args.kind else None,
                tags=args.tags,
                k=args.limit,
            )
            for h in hits:
                print(f"{h.item.id}  {h.item.kind:8} {h.item.scope:7} {h.item.short(120)}")
        elif args.action == "forget":
            target = " ".join(args.text)
            n = await mem.forget(
                target if target.startswith("mem_") else None, query=None if target.startswith("mem_") else target
            )
            print(f"forgot {n} item(s)")
        elif args.action == "summarize":
            print(await mem.summarize(" ".join(args.text), limit=args.limit))
        elif args.action == "consolidate":
            _print(await mem.consolidate(older_than_days=args.older_than), args.json)
        elif args.action == "decay":
            print(f"archived {await mem.decay(half_life_days=args.half_life)} item(s)")
        elif args.action == "stats":
            _print(await mem.stats(), True)
        elif args.action == "export":
            items = await mem.store.all_items()  # type: ignore[attr-defined]
            for it in items:
                print(it.model_dump_json())
        elif args.action == "import":
            from ..memory.base import MemoryItem

            n = 0
            for line in Path(" ".join(args.text)).read_text().splitlines():
                if line.strip():
                    await mem.store.add(MemoryItem.model_validate_json(line))
                    n += 1
            print(f"imported {n} item(s)")
    finally:
        await mem.close()
    return 0


# ---------------------------------------------------------------------- trace
def cmd_trace(args: argparse.Namespace) -> int:
    from ..harness.checkpoint import list_runs, runs_root
    from ..observability.viewer import render_stats, render_tree

    if args.action == "list":
        runs = list_runs(args.project)
        if args.json:
            _print(runs, True)
            return 0
        for r in runs[: args.limit]:
            cost = r.get("cost_usd")
            print(
                f"{r['run_id']:28} {r.get('workflow') or ''!s:24} {r.get('status') or ''!s:10} "
                f"{f'${cost:.4f}' if isinstance(cost, (int, float)) else '':>9}"
            )
        return 0
    path = _trace_path(args.run or "latest", args.project, runs_root)
    if path is None:
        print(f"no trace for '{args.run}' (sky-agents trace list --project {args.project})", file=sys.stderr)
        return 1
    from ..observability.tracing import load_trace

    spans = load_trace(path)
    if args.json:
        _print(spans, True)
    elif args.action == "stats":
        print(render_stats(spans))
    else:
        print(render_tree(spans, verbose=args.verbose, max_depth=args.depth))
    return 0


def _trace_path(run: str, project: str, runs_root: Any) -> Path | None:
    p = Path(run)
    if p.is_file():
        return p
    root = runs_root(project)
    if run == "latest":
        dirs = sorted([d for d in root.glob("*") if (d / "trace.jsonl").exists()], key=lambda d: d.stat().st_mtime)
        return dirs[-1] / "trace.jsonl" if dirs else None
    cand = root / run / "trace.jsonl"
    return cand if cand.exists() else None


# ---------------------------------------------------------------------- tools serving
async def cmd_serve_mcp(args: argparse.Namespace) -> int:
    from ..adapters.mcp_server import serve_stdio
    from ..memory import Memory

    mem = Memory.open(args.project, agent=args.agent or "", global_memory=not args.no_global)
    try:
        await serve_stdio(
            mem.tools(),
            name="sky-agents",
            agent=args.agent or "",
            instructions="Shared long-term memory of this Skywalker project's agents: recall before you "
            "start, remember durable facts and decisions.",
        )
    finally:
        await mem.close()
    return 0


async def cmd_host_tools(args: argparse.Namespace) -> int:
    from ..memory import Memory

    engine = await _engine(args)
    mem = Memory.open(args.project, global_memory=not args.no_global)
    server = await engine.host_tools(mem.tools(), label="sky-agents memory")
    print(
        f"serving {', '.join('py_' + t.name for t in mem.tools())} as host {server.host_id}; Ctrl-C to stop", flush=True
    )
    try:
        while True:
            await asyncio.sleep(3600)
    except (asyncio.CancelledError, KeyboardInterrupt):
        pass
    finally:
        await engine.close()
        await mem.close()
    return 0


# ---------------------------------------------------------------------- misc
def cmd_cards(args: argparse.Namespace) -> int:
    from ..comms.cards import export_cards, load_profiles

    profiles = load_profiles(args.project)
    if not profiles:
        print(f"no agents in {args.project}/agents (studio_team_template creates a team)", file=sys.stderr)
        return 1
    written = export_cards(profiles, args.out, base_url=args.base_url)
    print(f"wrote {len(written)} agent card(s) to {args.out}")
    return 0


def cmd_new(args: argparse.Namespace) -> int:
    from ..plugins.template import scaffold_plugin

    root = scaffold_plugin(args.name, args.dir)
    print(f"created {root}\n  cd {root} && pip install -e . && pytest\n  sky-agents plugins   # lists what it adds")
    return 0


def cmd_plugins(args: argparse.Namespace) -> int:
    from ..plugins.builtins import ensure_builtins

    reg = ensure_builtins()
    summary = reg.summary()
    if reg.failed:
        summary["failed"] = reg.failed  # type: ignore[assignment]
    _print(summary, True)
    return 0


def cmd_gen_tools(args: argparse.Namespace) -> int:
    from ..engine import codegen
    from ..engine.process import find_binary

    tools, version = codegen.tools_from_binary(find_binary(args.binary))
    out = Path(args.out) if args.out else Path(__file__).resolve().parents[1] / "engine" / "_generated.py"
    out.write_text(codegen.generate(tools, version))
    print(f"wrote {out} ({len(tools)} tools, engine {version})")
    return 0


async def cmd_eval(args: argparse.Namespace) -> int:
    from ..harness.evals import run_eval
    from ..harness.evals_spec import load_eval

    subject, scenarios, name = load_eval(args.spec, provider=_provider(args), project=args.project)

    async def factory() -> Any:
        return await _engine(args)

    report = await run_eval(subject, scenarios, engine_factory=factory, name=name, out=args.out)
    print(report.model_dump_json(indent=2) if args.json else report.summary())
    return 0 if report.passed == report.total else 1


async def cmd_doctor(args: argparse.Namespace) -> int:
    import importlib.util
    import sqlite3

    from ..engine.process import default_editor_socket, find_binary

    checks: dict[str, Any] = {"version": __version__, "python": sys.version.split()[0]}
    try:
        checks["skywalker_binary"] = find_binary(args.binary)
    except Exception as e:
        checks["skywalker_binary"] = f"missing: {e}"
    sock = default_editor_socket()
    checks["editor_socket"] = sock if os.path.exists(sock) else "not running"
    checks["ANTHROPIC_API_KEY"] = "set" if os.environ.get("ANTHROPIC_API_KEY") else "not set"
    checks["OPENAI_API_KEY"] = "set" if os.environ.get("OPENAI_API_KEY") else "not set"
    try:
        sqlite3.connect(":memory:").execute("create virtual table t using fts5(x)")
        checks["sqlite_fts5"] = "ok"
    except sqlite3.Error as e:
        checks["sqlite_fts5"] = f"unavailable: {e}"
    for mod in ("anthropic", "openai", "mcp", "sqlite_vec", "opentelemetry", "langchain_core", "PIL"):
        checks[f"extra:{mod}"] = "installed" if importlib.util.find_spec(mod) else "-"
    _print(checks, True)
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sky-agents", description="Teams of AI agents on the Skywalker engine.")
    p.add_argument("--version", action="version", version=f"sky-agents {__version__}")
    p.add_argument("--log", default=os.environ.get("SKY_AGENTS_LOG", "warning"), help="log level")
    sub = p.add_subparsers(dest="command", required=True)

    r = sub.add_parser("run", help="run a workflow (YAML/JSON, an engine loop JSON, or module:attribute)")
    r.add_argument("workflow")
    r.add_argument("--input", "-i", action="append", metavar="KEY=VALUE", help="workflow input (JSON values ok)")
    r.add_argument("--resume", metavar="RUN_ID", help="resume a run from its checkpoint")
    r.add_argument(
        "--approve",
        choices=["auto", "deny", "terminal", "studio"],
        help="who approves gated actions (default: the editor when attached, else deny)",
    )
    r.add_argument("--max-cost", type=float, help="run budget in USD")
    r.add_argument("--max-minutes", type=float, help="run time budget")
    r.add_argument("--record", metavar="CASSETTE", help="record model and tool calls (JSONL)")
    r.add_argument("--replay", metavar="CASSETTE", help="replay a recording (no model calls, no engine edits)")
    r.add_argument("--otel", metavar="ENDPOINT", help="also export spans over OTLP/HTTP")
    r.add_argument("--no-memory", action="store_true")
    r.add_argument("--json", action="store_true")
    _add_engine_args(r)
    _add_model_args(r)

    c = sub.add_parser("chat", help="talk to a roster member (its brief, tools and memory)")
    c.add_argument("role")
    c.add_argument("--no-memory", action="store_true")
    _add_engine_args(c)
    _add_model_args(c)

    m = sub.add_parser(
        "memory", help="shared memory: add, search, list, forget, summarize, consolidate, decay, stats, export, import"
    )
    m.add_argument(
        "action",
        choices=[
            "add",
            "search",
            "recall",
            "list",
            "forget",
            "summarize",
            "consolidate",
            "decay",
            "stats",
            "export",
            "import",
        ],
    )
    m.add_argument("text", nargs="*")
    m.add_argument("--project", default=os.environ.get("SKY_PROJECT", "."))
    m.add_argument("--agent", help="read/write as this agent (its private memories)")
    m.add_argument("--team", action="append")
    m.add_argument("--scope", choices=["agent", "team", "project", "global"])
    m.add_argument("--kind", choices=["episodic", "fact", "note", "artifact"])
    m.add_argument("--tags", nargs="*")
    m.add_argument("--importance", type=float, default=0.5)
    m.add_argument("--limit", type=int, default=10)
    m.add_argument("--older-than", type=float, default=1.0, help="consolidate: days")
    m.add_argument("--half-life", type=float, default=30.0, help="decay: days")
    m.add_argument("--semantic", action="store_true", help="also use (hashing) embeddings")
    m.add_argument("--no-global", action="store_true")
    m.add_argument("--json", action="store_true")

    t = sub.add_parser("trace", help="read run traces: list, show RUN|latest|FILE, stats")
    t.add_argument("action", choices=["list", "show", "stats"])
    t.add_argument("run", nargs="?")
    t.add_argument("--project", default=os.environ.get("SKY_PROJECT", "."))
    t.add_argument("--verbose", "-v", action="store_true")
    t.add_argument("--depth", type=int, default=12)
    t.add_argument("--limit", type=int, default=20)
    t.add_argument("--json", action="store_true")

    s = sub.add_parser("serve-mcp", help="serve shared memory as an MCP server on stdio (Claude Code, Codex, ...)")
    s.add_argument("--project", default=os.environ.get("SKY_PROJECT", "."))
    s.add_argument("--agent", help="identity of the connecting agent (private memories)")
    s.add_argument("--no-global", action="store_true")

    h = sub.add_parser("host-tools", help="serve shared memory to every agent on the engine as py_memory_* tools")
    h.add_argument("--no-global", action="store_true")
    _add_engine_args(h)

    cards = sub.add_parser("cards", help="export A2A-style agent cards for the roster")
    cards.add_argument("--project", default=os.environ.get("SKY_PROJECT", "."))
    cards.add_argument("--out", default="agent-cards")
    cards.add_argument("--base-url", default="")

    n = sub.add_parser("new", help="scaffold an extension (sky-agents new plugin NAME)")
    n.add_argument("what", choices=["plugin"])
    n.add_argument("name")
    n.add_argument("--dir", default=".")

    sub.add_parser("plugins", help="list providers, stores, tools, patterns, hooks... and loaded plugins")

    g = sub.add_parser("gen-tools", help="regenerate the typed engine tool wrappers from `skywalker tools --json`")
    g.add_argument("--binary")
    g.add_argument("--out")

    e = sub.add_parser("eval", help="run an eval spec (YAML/JSON: workflow + scenarios + checks)")
    e.add_argument("spec")
    e.add_argument("--out", help="write the report JSON here")
    e.add_argument("--json", action="store_true")
    _add_engine_args(e)
    _add_model_args(e)

    d = sub.add_parser("doctor", help="check the binary, editor socket, keys (presence only) and extras")
    d.add_argument("--binary")
    return p


def main(argv: list[str] | None = None) -> int:
    import logging

    args = build_parser().parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, str(args.log).upper(), logging.WARNING), format="%(levelname)s %(name)s: %(message)s"
    )
    handlers: dict[str, Any] = {
        "run": cmd_run,
        "chat": cmd_chat,
        "memory": cmd_memory,
        "trace": cmd_trace,
        "serve-mcp": cmd_serve_mcp,
        "host-tools": cmd_host_tools,
        "cards": cmd_cards,
        "new": cmd_new,
        "plugins": cmd_plugins,
        "gen-tools": cmd_gen_tools,
        "eval": cmd_eval,
        "doctor": cmd_doctor,
    }
    fn = handlers[args.command]
    try:
        out = fn(args)
        if asyncio.iscoroutine(out):
            out = asyncio.run(out)
        return int(out or 0)
    except KeyboardInterrupt:
        return 130
    except Exception as e:  # a CLI prints errors, it does not dump tracebacks (use --log debug for those)
        logging.getLogger("skywalker_agents").debug("failed", exc_info=True)
        print(f"sky-agents {args.command}: {type(e).__name__}: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
