#!/usr/bin/env python3
"""Check that every code sample on the documentation site is real.

    python3 website/scripts/check_snippets.py                         # tool calls, CLI lines, hygiene (no engine needed)
    python3 website/scripts/check_snippets.py --cli build/release/bin/skywalker
                                                                      # ... and compile every Wander block too

Checks every Markdown page under website/docs:
  1. tool calls: in ```tool, ```text, ```jsonc and plain fences, every `tool_name {args}` must name a real tool and use
     real arguments, enum values and types (batch operations are checked recursively); ```json blocks of the form
     {"tool": name, "args": {...}} are checked the same way; `skywalker call NAME '{json}'` lines too;
  2. CLI lines in ```bash / ```sh fences: `skywalker <command>` must be a real command;
  3. inline `tool_like_names` that look like engine tools must exist (tools, arguments, enum values, components,
     fields and Wander builtins count as known words);
  4. ```wander blocks compile with `skywalker check` (only with --cli);
  5. hygiene over every file of the site's sources: no absolute home or temp paths, e-mail addresses or API keys,
     and no names of other engines.
Validation of tool arguments reuses integrations/check_skills.py. Exit status 1 lists every problem.
"""
from __future__ import annotations

import argparse
import codecs
import importlib.util
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SITE = ROOT / "website"
DOCS = SITE / "docs"
DATA = SITE / "data"

_spec = importlib.util.spec_from_file_location("check_skills", ROOT / "integrations" / "check_skills.py")
skills = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(skills)

# Words that look like tool names in inline code but are not tools (file names, values, Wander names...).
ALLOW = set(skills.ALLOW) | {
    "sky_api", "sky_call_tool", "sky_engine_create", "sky_update", "sky_string_free", "sky_tools_list",
    "skywalker_tests", "skywalker_dcc", "skywalker_bridge", "scene_camera", "physics_world", "nav_agent", "sprite_anim",
    "ui_canvas", "camera2d", "light2d", "on_surface", "min_distance", "max_iterations", "studio_id", "loop_member",
    "save_dir", "debug_view", "focus_distance", "tilt_shift", "include_image", "save_path", "camera_entity",
    "frame_handlers", "align_sun_to_hdri", "create_material", "update_references", "skip_existing", "all_assets",
    "build_native", "dry_run", "bundle_id", "render_layers", "wander_bench", "scene_overview", "entity_get",
    "tool_warn", "tool_fail", "tool_actor",  # builtins that exist only inside custom tool code (docs/CUSTOM_TOOLS.md)
}

# Names of other engines are stored ROT13-encoded so they never appear in the site's sources themselves.
_FORBIDDEN = [codecs.decode(w, "rot13") for w in (
    "tbqbg", "havgl", "haerny", "pelratvar", "tnzrznxre", "sebfgovgr", "ebokbk", "qrsbyq", "yhzorelneq", "b3qr",
    "orib", "zbabtnzr", "oyhrcevag", "avntnen", "anavgr", "sbegavgr", "hr4", "hr5")]
FORBIDDEN_RE = re.compile(r"(?i)\b(" + "|".join(map(re.escape, _FORBIDDEN)) + r")\b")
PRIVACY = [
    (re.compile(r"/Users/(?!me\b)[A-Za-z0-9._-]+"), "absolute home path (use /Users/me/... or ~/...)"),
    (re.compile(r"/private/(?:tmp|var)/|/var/f[o]lders/"), "machine temp path"),
    (re.compile(r"[A-Za-z0-9._%+-]+@(?:gmail|outlook|hotmail|yahoo|icloud)\.[a-z]+", re.I), "e-mail address"),
    (re.compile(r"\bsk-(?:ant-)?[A-Za-z0-9_-]{16,}"), "API key"),
]
HYGIENE_SUFFIXES = {".md", ".yml", ".yaml", ".py", ".css", ".html", ".js", ".json", ".txt", ".svg"}

FENCE_RE = re.compile(r"^(?P<indent>[ \t]*)(?P<fence>`{3,})(?P<lang>[\w+-]*)[^\n]*\n(?P<body>.*?)^(?P=indent)(?P=fence)[ \t]*$",
                      re.M | re.S)


def load_tools() -> dict:
    return skills.load_tools(DATA / "tools.json")


def known_words(tools: dict) -> set[str]:
    words = skills.known_words(tools)
    comps = json.loads((DATA / "components.json").read_text())
    for name, schema in comps.items():
        words.add(name)
        words.update(schema.get("properties", {}))
        for sub in schema.get("properties", {}).values():
            if isinstance(sub, dict):
                words.update(e for e in sub.get("enum", []) if isinstance(e, str))
    wander = json.loads((DATA / "wander.json").read_text())
    for entries in wander["categories"].values():
        words.update(e["name"] for e in entries if "name" in e)
        words.update(p["name"] for e in entries for p in e.get("params", []))
    return words


def cli_commands() -> set[str]:
    usage = json.loads((DATA / "cli.json").read_text())["usage"]
    return set(re.findall(r"^\s*skywalker ([a-z]+)", usage, re.M)) | {"help"}


def strip_line_comments(text: str) -> str:
    """Drop // and -- comments outside strings (the skills checker already handles #)."""
    out, i, in_str, quote = [], 0, False, ""
    while i < len(text):
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < len(text):
                out.append(text[i + 1])
                i += 1
            elif c == quote:
                in_str = False
        elif c in "\"'":
            in_str, quote = True, c
            out.append(c)
        elif text.startswith("//", i) or (text.startswith("--", i) and (i == 0 or text[i - 1] in " \t")):
            while i < len(text) and text[i] != "\n":
                i += 1
            continue
        else:
            out.append(c)
        i += 1
    return "".join(out)


def skills_closest(name: str, tools: dict) -> str:
    import difflib
    match = difflib.get_close_matches(name, list(tools), n=1)
    return f" (did you mean `{match[0]}`?)" if match else ""


def check_calls_in(block: str, tools: dict, where: str, problems: list[str]) -> int:
    count = 0
    for m in re.finditer(r"(?<![A-Za-z0-9_./-])([a-z][a-z0-9_]*)\s*\{", block):
        name = m.group(1)
        if name not in tools:
            continue
        end = skills.balanced(block, m.end() - 1)
        if end < 0:
            problems.append(f"{where}: unbalanced braces in the {name} example")
            continue
        raw = block[m.end() - 1:end]
        try:
            args = json.loads(skills.js_to_json(skills.strip_comments(strip_line_comments(raw))))
        except json.JSONDecodeError as e:
            problems.append(f"{where}: {name} example is not valid JSON-ish ({e.msg}): {raw[:70]!r}")
            continue
        skills.check_call(name, args, tools, where, problems)
        count += 1
    return count


def check_page(path: Path, tools: dict, words: set[str], commands: set[str], wander_blocks: list, problems: list[str]) -> int:
    text = path.read_text()
    rel = path.relative_to(SITE)
    calls = 0
    fences = []
    for m in FENCE_RE.finditer(text):
        fences.append((m.start(), m.end()))
        lang = m.group("lang").lower()
        indent = len(m.group("indent"))
        body = "\n".join(l[indent:] if l[:indent].strip() == "" else l for l in m.group("body").splitlines())
        line = text[:m.start()].count("\n") + 2
        where = f"{rel}:{line}"
        if lang == "tool":
            # In tool fences every call must name a real tool (other fences may mention JSON-ish non-tools).
            for i, l in enumerate(body.splitlines()):
                um = re.match(r"\s*([a-z][a-z0-9_]*)\s*\{", l)
                if um and um.group(1) not in tools:
                    hint = skills_closest(um.group(1), tools)
                    problems.append(f"{rel}:{line + i}: unknown tool `{um.group(1)}`{hint}")
        if lang in ("tool", "text", "", "jsonc", "js", "javascript"):
            calls += check_calls_in(body, tools, where, problems)
        elif lang == "json":
            try:
                doc = json.loads(body)
            except json.JSONDecodeError:
                continue
            items = doc if isinstance(doc, list) else [doc]
            for item in items:
                if isinstance(item, dict) and isinstance(item.get("tool"), str) and "args" in item:
                    if item["tool"] not in tools:
                        problems.append(f"{where}: unknown tool {item['tool']!r}")
                    else:
                        skills.check_call(item["tool"], item["args"], tools, where, problems)
                        calls += 1
        elif lang in ("bash", "sh", "shell", "console", "zsh"):
            for i, l in enumerate(body.splitlines()):
                cm = re.search(r"(?:^|[\s/(])skywalker\s+([a-z][a-z-]*)", l)
                if cm and cm.group(1) not in commands and not l.lstrip().startswith("#"):
                    problems.append(f"{rel}:{line + i}: `skywalker {cm.group(1)}` is not a CLI command")
                tm = re.search(r"skywalker\s+call\s+([a-z0-9_]+)\s+'((?:[^']|'\\'')*)'", l)
                if tm:
                    name = tm.group(1)
                    raw_args = tm.group(2).replace("'\\''", "'")
                    if name not in tools:
                        problems.append(f"{rel}:{line + i}: unknown tool {name!r}")
                        continue
                    try:
                        skills.check_call(name, json.loads(raw_args), tools, f"{rel}:{line + i}", problems)
                        calls += 1
                    except json.JSONDecodeError as e:
                        problems.append(f"{rel}:{line + i}: arguments of {name} are not JSON ({e.msg})")
        elif lang == "wander":
            wander_blocks.append((where, body))
    # inline code that looks like a tool
    prefixes = skills.FAMILIES | {n.split("_")[0] for n in tools}
    for m in re.finditer(r"(?<!`)`([^`\n]+)`(?!`)", text):
        if any(a <= m.start() < b for a, b in fences):
            continue
        token = re.split(r"[\s{(]", m.group(1))[0].rstrip(",.;:")
        if (re.fullmatch(r"[a-z]+(_[a-z0-9]+)+", token) and token.split("_")[0] in prefixes and token not in words
                and token not in ALLOW):
            line = text[:m.start()].count("\n") + 1
            problems.append(f"{rel}:{line}: `{token}` looks like an engine tool but no such tool/argument exists")
    return calls


def check_wander(cli: str, blocks: list, problems: list[str]) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        for i, (where, body) in enumerate(blocks):
            f = Path(tmp) / f"snippet_{i}.wander"
            f.write_text(body)
            r = subprocess.run([cli, "check", str(f), "--project", tmp], capture_output=True, text=True)
            if r.returncode != 0:
                msg = (r.stdout + r.stderr).replace(str(f), "snippet").strip()
                problems.append(f"{where}: Wander block does not compile:\n    " + msg.replace("\n", "\n    "))


def check_hygiene(problems: list[str]) -> int:
    n = 0
    for path in sorted(SITE.rglob("*")):
        if not path.is_file() or path.suffix not in HYGIENE_SUFFIXES or "site" in path.relative_to(SITE).parts[:1]:
            continue
        n += 1
        text = path.read_text(errors="replace")
        rel = path.relative_to(ROOT)
        for m in FORBIDDEN_RE.finditer(text):
            line = text[:m.start()].count("\n") + 1
            problems.append(f"{rel}:{line}: names another engine ({m.group(0)!r}); describe Skywalker on its own terms")
        for rx, what in PRIVACY:
            for m in rx.finditer(text):
                line = text[:m.start()].count("\n") + 1
                problems.append(f"{rel}:{line}: {what}: {m.group(0)!r}")
    return n


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", help="skywalker binary: also compile every ```wander block")
    opts = ap.parse_args()
    tools = load_tools()
    words = known_words(tools)
    commands = cli_commands()
    problems: list[str] = []
    wander_blocks: list = []
    pages = sorted(DOCS.rglob("*.md"))
    calls = sum(check_page(p, tools, words, commands, wander_blocks, problems) for p in pages)
    if opts.cli:
        check_wander(str(Path(opts.cli).resolve()), wander_blocks, problems)
    files = check_hygiene(problems)
    for p in problems:
        print(p)
    if problems:
        print(f"{len(problems)} problem(s)", file=sys.stderr)
        return 1
    wander_note = f"{len(wander_blocks)} Wander blocks compiled" if opts.cli else f"{len(wander_blocks)} Wander blocks (not compiled: pass --cli)"
    print(f"snippets OK: {len(pages)} pages, {calls} tool calls checked against {len(tools)} tools, {wander_note}, "
          f"{files} files clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
