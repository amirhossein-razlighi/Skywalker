#!/usr/bin/env python3
"""Dump the engine's live reference data into website/data/*.json.

    python3 website/scripts/dump_data.py --cli build/release/bin/skywalker            # write website/data
    python3 website/scripts/dump_data.py --cli build/headless/bin/skywalker --out DIR # write somewhere else

The site's reference pages are generated from these files by gen_reference.py, so the Pages workflow never has to
build the C++ engine. Sources:

  tools.json       `skywalker tools --json` (every tool: name, description, schema, annotations, category)
  components.json  `skywalker call component_schema` (every reflected component and the environment)
  wander.json      `skywalker call wander_reference` (the guide) plus one structured call per builtin category
  cli.json         the CLI's usage text and the help of its subcommands
  capi.json        the declarations and comments of engine/capi/include/sky_api.h

Output is deterministic and scrubbed of machine-specific paths. Standard library only.
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


def call_json(cli: str, tool: str, args: dict | None = None):
    text = run(cli, "call", tool, json.dumps(args or {}))
    start = text.find("{")
    if start < 0:
        raise SystemExit(f"{tool}: no JSON in output:\n{text[:400]}")
    return json.loads(text[start:])


def dump_tools(cli: str) -> dict:
    data = json.loads(run(cli, "tools", "--json"))
    return {"version": run(cli, "version").strip().split()[-1], "tools": data["tools"]}


def dump_components(cli: str) -> dict:
    return call_json(cli, "component_schema")


def dump_wander(cli: str) -> dict:
    guide = run(cli, "call", "wander_reference")
    categories = re.findall(r"^\s{2}\[([a-z0-9_]+)\]\s*$", guide, re.M)
    entries = {}
    for cat in categories:
        entries[cat] = call_json(cli, "wander_reference", {"topic": cat})["entries"]
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
    dumps = {
        "tools.json": dump_tools(cli),
        "components.json": dump_components(cli),
        "wander.json": dump_wander(cli),
        "cli.json": dump_cli(cli),
        "capi.json": dump_capi(),
    }
    for name, data in dumps.items():
        text = scrub(json.dumps(data, indent=1, ensure_ascii=False, sort_keys=False)) + "\n"
        (out / name).write_text(text)
    print(f"wrote {', '.join(dumps)} to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
