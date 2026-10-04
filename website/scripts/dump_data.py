#!/usr/bin/env python3
"""Dump the engine's live reference data into website/data/*.json.

    python3 website/scripts/dump_data.py --cli build/release/bin/skywalker            # write website/data
    python3 website/scripts/dump_data.py --cli build/headless/bin/skywalker --out DIR # write somewhere else

The site's reference pages are generated from these files by gen_reference.py, so the Pages workflow never has to
build the C++ engine. Sources:

  tools.json       MCP tools/list (every tool: name, description, schema, annotations, category; = `skywalker tools --json`)
  components.json  the component_schema tool (every reflected component and the environment)
  wander.json      the wander_reference tool (the guide) plus one structured call per builtin category
  cli.json         the CLI's usage text and the help of its subcommands
  capi.json        the declarations and comments of engine/capi/include/sky_api.h

Tools are called through one headless `skywalker mcp` session on an empty temporary project. Output is deterministic
and scrubbed of machine-specific paths. Standard library only.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "website" / "data"

# Absolute paths of the machine that ran the dump never reach the site. Placeholders used in examples
# (/Users/me/...) are kept.
_HOME_RE = re.compile(r"/Users/(?!me/|me\b)[^/\s\"']+")
_TMP_RE = re.compile(r"/private/(?:var|tmp)/[^\s\"']*|/var/f[o]lders/[^\s\"']*")


def scrub(text: str) -> str:
    text = text.replace(str(ROOT), "<repo>")
    text = _HOME_RE.sub("~", text)
    return _TMP_RE.sub("<tmp>", text)


def run(cli: str, *args: str) -> str:
    # Run outside the repository so nothing (caches, scenes) is written into it.
    proc = subprocess.run([cli, *args], capture_output=True, text=True, cwd=tempfile.gettempdir())
    out = proc.stdout
    if proc.returncode not in (0, 2):  # usage/help screens exit with 2
        raise SystemExit(f"`skywalker {' '.join(args)}` failed ({proc.returncode}):\n{proc.stderr}")
    return out if out.strip() else proc.stderr


class Session:
    """One headless `skywalker mcp` process for all tool calls (fast, and only one engine start)."""

    def __init__(self, cli: str):
        self.tmp = tempfile.TemporaryDirectory()
        self.proc = subprocess.Popen([cli, "mcp", "--project", self.tmp.name], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, cwd=self.tmp.name)
        self.ids = 0
        self.server = self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                                  "clientInfo": {"name": "site-dump", "version": "1"}})["serverInfo"]
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        self.proc.stdin.flush()

    def request(self, method: str, params: dict):
        self.ids += 1
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.ids, "method": method, "params": params}) + "\n")
        self.proc.stdin.flush()
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise SystemExit(f"skywalker mcp exited during {method}")
            msg = json.loads(line)
            if msg.get("id") == self.ids:
                if "error" in msg:
                    raise SystemExit(f"{method}: {msg['error']}")
                return msg["result"]

    def call(self, tool: str, args: dict | None = None):
        result = self.request("tools/call", {"name": tool, "arguments": args or {}})
        if result.get("isError"):
            raise SystemExit(f"{tool}: {result}")
        return result

    def close(self) -> None:
        self.proc.stdin.close()
        self.proc.wait(timeout=60)
        self.tmp.cleanup()


def dump_tools(mcp: Session) -> dict:
    return {"version": mcp.server["version"], "tools": mcp.request("tools/list", {})["tools"]}


def dump_components(mcp: Session) -> dict:
    return mcp.call("component_schema")["structuredContent"]


def dump_wander(mcp: Session) -> dict:
    guide = "".join(c.get("text", "") for c in mcp.call("wander_reference")["content"] if c.get("type") == "text")
    categories = re.findall(r"^\s{2}\[([a-z0-9_]+)\]\s*$", guide, re.M)
    entries = {}
    for cat in categories:
        entries[cat] = mcp.call("wander_reference", {"topic": cat})["structuredContent"]["entries"]
    return {"guide": guide, "categories": entries}


def dump_cli(cli: str) -> dict:
    commands = {}
    for name, args in [("build", ["build", "--help"]), ("movie", ["movie", "--help"]), ("setup", ["setup", "--help"]),
                       ("studio", ["studio", "help"])]:
        commands[name] = run(cli, *args)
    return {"usage": run(cli, "help"), "commands": commands}


def dump_capi() -> dict:
    header = (ROOT / "engine" / "capi" / "include" / "sky_api.h").read_text()
    intro = re.match(r"/\*(.*?)\*/", header, re.S).group(1)
    intro = "\n".join(line.strip().lstrip("*").strip() for line in intro.strip().splitlines())
    body = header[header.find('extern "C"'):]
    sections, current, pending = [], None, []
    lines = body.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        m = re.match(r"/\*\s*(.+?)\s*-{3,}\s*\*/", line)
        if m:
            current = {"title": m.group(1).rstrip(". "), "items": []}
            sections.append(current)
            pending = []
        elif line.startswith("/*"):
            comment = [line]
            while not comment[-1].endswith("*/") and i + 1 < len(lines):
                i += 1
                comment.append(lines[i].strip())
            text = " ".join(comment)
            text = re.sub(r"^/\*|\*/$", "", text).strip()
            pending.append(re.sub(r"\s+", " ", text))
        elif line.startswith("typedef struct") and "{" in line:
            decl = [lines[i]]
            while "}" not in lines[i]:
                i += 1
                decl.append(lines[i])
            current["items"].append({"kind": "struct", "decl": "\n".join(decl), "doc": " ".join(pending)})
            pending = []
        elif line and not line.startswith(("#", "}", "extern")) and current is not None:
            decl = line
            while not decl.rstrip().endswith(";") and i + 1 < len(lines):
                i += 1
                decl += " " + lines[i].strip()
            trailing = ""
            tm = re.search(r";\s*/\*(.*?)\*/\s*$", decl)
            if tm:
                trailing = tm.group(1).strip()
                decl = decl[: tm.start() + 1]
            decl = re.sub(r"\s+", " ", decl.strip())
            name = re.search(r"(\w+)\s*\(", decl)
            kind = "typedef" if decl.startswith("typedef") else "function"
            doc = " ".join(pending + ([trailing] if trailing else []))
            current["items"].append({"kind": kind, "name": name.group(1) if name else decl, "decl": decl, "doc": doc})
            pending = []
        i += 1
    return {"intro": intro, "sections": sections}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True, help="path to the skywalker binary")
    ap.add_argument("--out", default=str(DATA), help="output folder (default website/data)")
    opts = ap.parse_args()
    cli = str(Path(opts.cli).resolve())
    out = Path(opts.out)
    out.mkdir(parents=True, exist_ok=True)
    mcp = Session(cli)
    try:
        dumps = {
            "tools.json": dump_tools(mcp),
            "components.json": dump_components(mcp),
            "wander.json": dump_wander(mcp),
            "cli.json": dump_cli(cli),
            "capi.json": dump_capi(),
        }
    finally:
        mcp.close()
    for name, data in dumps.items():
        text = scrub(json.dumps(data, indent=1, ensure_ascii=False, sort_keys=False)) + "\n"
        (out / name).write_text(text)
    print(f"wrote {', '.join(dumps)} to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
