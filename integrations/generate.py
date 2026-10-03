#!/usr/bin/env python3
"""Generate the Claude Code / Codex / Gemini CLI / Cursor integrations from one source.

Single source of truth: integrations/skills-src/
    skills/<name>/SKILL.md (+ references/**)   portable Agent Skills (name + description frontmatter)
    agents/<name>.md  + agents/_protocol.md      studio-role subagents ({{PROTOCOL}} is replaced)
    commands/<name>.md                           slash-command workflows ($ARGUMENTS)
    context.md                                   project guidance block (AGENTS.md / GEMINI.md / rules)
    meta.json                                    plugin metadata and the MCP server entry

Outputs (committed; the tests fail when they are stale):
    integrations/claude-code/   plugin (.claude-plugin/, skills/, agents/, commands/, .mcp.json)
    .claude-plugin/marketplace.json (repo root marketplace pointing at integrations/claude-code)
    integrations/codex/         AGENTS.md, .agents/skills/, .codex/{config.toml,agents/*.toml}
    integrations/gemini/        gemini-extension.json, GEMINI.md, skills/, agents/, commands/
    integrations/cursor/        .cursor/{mcp.json,rules/,skills/,agents/,commands/}

Usage:
    python3 integrations/generate.py            write everything
    python3 integrations/generate.py --check    exit 1 (and list the differences) if anything is stale
No dependencies beyond the Python 3 standard library.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
SRC = HERE / "skills-src"

BEGIN = "<!-- BEGIN SKYWALKER (managed by `skywalker setup`; edit outside these markers) -->"
END = "<!-- END SKYWALKER -->"

# Generated roots per tool, relative to the repo. Everything below these directories that is not produced
# by the generator is reported as stale (so a deleted skill disappears from every output).
CLAUDE = HERE / "claude-code"
CODEX = HERE / "codex"
GEMINI = HERE / "gemini"
CURSOR = HERE / "cursor"

OWNED_DIRS = [
    CLAUDE / "skills", CLAUDE / "agents", CLAUDE / "commands", CLAUDE / ".claude-plugin",
    CODEX / ".agents" / "skills", CODEX / ".codex" / "agents",
    GEMINI / "skills", GEMINI / "agents", GEMINI / "commands",
    CURSOR / ".cursor" / "skills", CURSOR / ".cursor" / "agents", CURSOR / ".cursor" / "commands",
    CURSOR / ".cursor" / "rules",
    REPO / ".claude-plugin",
]
OWNED_FILES = [
    CLAUDE / ".mcp.json",
    CODEX / "AGENTS.md", CODEX / ".codex" / "config.toml",
    GEMINI / "gemini-extension.json", GEMINI / "GEMINI.md",
    CURSOR / ".cursor" / "mcp.json",
]


# ---------------------------------------------------------------------------------------------------------
# Parsing helpers


def split_frontmatter(text: str, path: Path) -> tuple[dict[str, str], str]:
    """Flat `key: value` frontmatter (values may contain colons). Returns (meta, body)."""
    if not text.startswith("---\n"):
        raise SystemExit(f"{path}: missing frontmatter")
    end = text.find("\n---\n", 4)
    if end < 0:
        raise SystemExit(f"{path}: unterminated frontmatter")
    meta: dict[str, str] = {}
    for line in text[4:end].splitlines():
        if not line.strip():
            continue
        key, sep, value = line.partition(":")
        if not sep:
            raise SystemExit(f"{path}: bad frontmatter line {line!r}")
        meta[key.strip()] = value.strip()
    return meta, text[end + 5:].lstrip("\n")


def q(value: str) -> str:
    """A YAML-safe double-quoted scalar (JSON strings are valid YAML)."""
    return json.dumps(value, ensure_ascii=False)


def frontmatter(pairs: list[tuple[str, str | bool]]) -> str:
    lines = ["---"]
    for key, value in pairs:
        if isinstance(value, bool):
            lines.append(f"{key}: {'true' if value else 'false'}")
        elif key in ("name", "color", "model", "kind"):
            lines.append(f"{key}: {value}")
        else:
            lines.append(f"{key}: {q(value)}")
    lines.append("---")
    return "\n".join(lines) + "\n\n"


def toml_str(value: str) -> str:
    if "\n" not in value and '"' not in value and "\\" not in value:
        return f'"{value}"'
    if "'''" not in value:
        return "'''\n" + value + "'''"
    return '"""\n' + value.replace("\\", "\\\\").replace('"""', '\\"\\"\\"') + '"""'


def pretty(obj) -> str:
    return json.dumps(obj, indent=2, ensure_ascii=False) + "\n"


# ---------------------------------------------------------------------------------------------------------
# Source model


class Source:
    def __init__(self) -> None:
        self.meta = json.loads((SRC / "meta.json").read_text())
        self.context = (SRC / "context.md").read_text().rstrip() + "\n"
        self.protocol = (SRC / "agents" / "_protocol.md").read_text().rstrip() + "\n"
        self.skills: dict[str, dict] = {}
        for d in sorted((SRC / "skills").iterdir()):
            if not d.is_dir():
                continue
            skill_md = d / "SKILL.md"
            meta, body = split_frontmatter(skill_md.read_text(), skill_md)
            if meta.get("name") != d.name:
                raise SystemExit(f"{skill_md}: name {meta.get('name')!r} must equal the directory name {d.name!r}")
            if not meta.get("description"):
                raise SystemExit(f"{skill_md}: missing description")
            files = {p.relative_to(d).as_posix(): p.read_bytes() for p in sorted(d.rglob("*")) if p.is_file() and p.name != "SKILL.md"}
            self.skills[d.name] = {"meta": meta, "body": body, "files": files}
        self.agents: dict[str, dict] = {}
        for p in sorted((SRC / "agents").glob("*.md")):
            if p.name.startswith("_"):
                continue
            meta, body = split_frontmatter(p.read_text(), p)
            for key in ("name", "description", "studio_id", "role_title", "skills"):
                if not meta.get(key):
                    raise SystemExit(f"{p}: missing {key}")
            body = body.replace("{{PROTOCOL}}", self.protocol.rstrip())
            for key in ("ROLE_TITLE", "STUDIO_ID", "SKILLS"):
                value = meta[{"ROLE_TITLE": "role_title", "STUDIO_ID": "studio_id", "SKILLS": "skills"}[key]]
                if key == "SKILLS":
                    value = ", ".join(f"`{s.strip()}`" for s in value.split(","))
                body = body.replace("{{" + key + "}}", value)
            self.agents[meta["name"]] = {"meta": meta, "body": body}
        self.commands: dict[str, dict] = {}
        for p in sorted((SRC / "commands").glob("*.md")):
            meta, body = split_frontmatter(p.read_text(), p)
            if not meta.get("description"):
                raise SystemExit(f"{p}: missing description")
            self.commands[p.stem] = {"meta": meta, "body": body}

    # -- shared renderers ---------------------------------------------------------------------------------

    def skill_files(self, prefix: Path) -> dict[Path, bytes]:
        out: dict[Path, bytes] = {}
        for name, s in self.skills.items():
            head = frontmatter([("name", name), ("description", s["meta"]["description"])])
            out[prefix / name / "SKILL.md"] = (head + s["body"].rstrip() + "\n").encode()
            for rel, data in s["files"].items():
                out[prefix / name / rel] = data
        return out

    def mcp_servers(self) -> dict:
        m = self.meta["mcp"]
        return {m["server"]: {"command": m["command"], "args": m["args"]}}

    def context_block(self, addendum: str = "") -> str:
        return BEGIN + "\n" + self.context.rstrip() + "\n" + (("\n" + addendum.strip() + "\n") if addendum else "") + END + "\n"

    def skill_table(self) -> str:
        rows = ["| Skill | Use it for |", "|---|---|"]
        for name, s in self.skills.items():
            desc = s["meta"]["description"]
            first = desc.split(" - ")[0] if " - " in desc else desc
            rows.append(f"| `{name}` | {first.strip().rstrip('.')} |")
        return "\n".join(rows)


# ---------------------------------------------------------------------------------------------------------
# Targets


def gen_claude(src: Source) -> dict[Path, bytes]:
    m = src.meta
    out: dict[Path, bytes] = {}
    plugin = {
        "name": m["name"],
        "version": m["version"],
        "description": m["description"],
        "author": m["author"],
        "license": m["license"],
        "keywords": m["keywords"],
    }
    out[CLAUDE / ".claude-plugin" / "plugin.json"] = pretty(plugin).encode()
    entry = {"name": m["name"], "source": "./", "description": m["description"], "version": m["version"]}
    market = {
        "$schema": "https://anthropic.com/claude-code/marketplace.schema.json",
        "name": m["marketplace"],
        "description": "Skywalker game engine integrations for Claude Code",
        "owner": m["author"],
        "plugins": [entry],
    }
    out[CLAUDE / ".claude-plugin" / "marketplace.json"] = pretty(market).encode()
    root_entry = dict(entry, source="./integrations/claude-code")
    root_market = dict(market, plugins=[root_entry])
    out[REPO / ".claude-plugin" / "marketplace.json"] = pretty(root_market).encode()
    out[CLAUDE / ".mcp.json"] = pretty({"mcpServers": src.mcp_servers()}).encode()
    out.update(src.skill_files(CLAUDE / "skills"))
    for name, a in src.agents.items():
        head = frontmatter([("name", name), ("description", a["meta"]["description"]), ("color", a["meta"].get("color", "blue"))])
        out[CLAUDE / "agents" / f"{name}.md"] = (head + a["body"].rstrip() + "\n").encode()
    for name, c in src.commands.items():
        pairs: list[tuple[str, str | bool]] = [("description", c["meta"]["description"])]
        if c["meta"].get("argument-hint"):
            pairs.append(("argument-hint", c["meta"]["argument-hint"]))
        out[CLAUDE / "commands" / f"{name}.md"] = (frontmatter(pairs) + c["body"].rstrip() + "\n").encode()
    return out


def gen_codex(src: Source) -> dict[Path, bytes]:
    out: dict[Path, bytes] = {}
    addendum = (
        "Codex specifics: skills are in `.agents/skills` (invoke with `$skywalker-core`, or let Codex pick them from their descriptions); the studio roles are custom agents in `.codex/agents/` "
        "(`creative_director`, `level_designer`, `environment_artist`, `gameplay_programmer`, `technical_artist`, `sound_designer`, `playtester`, `critic`): spawn them explicitly and give each its "
        "assignment. Workflows are the skills named `skywalker-cmd-*` (`$skywalker-cmd-new-game <pitch>`). MCP tool calls can be slow (captures, playtests, DCC): keep `tool_timeout_sec` generous."
    )
    out[CODEX / "AGENTS.md"] = ("# Skywalker game project\n\n" + src.context_block(addendum)).encode()
    out.update(src.skill_files(CODEX / ".agents" / "skills"))
    for name, c in src.commands.items():
        skill = f"skywalker-cmd-{name}"
        body = c["body"].replace("$ARGUMENTS", "the request the user wrote after invoking this skill")
        head = frontmatter([("name", skill), ("description", "Workflow: " + c["meta"]["description"] + ". Invoke explicitly.")])
        out[CODEX / ".agents" / "skills" / skill / "SKILL.md"] = (head + body.rstrip() + "\n").encode()
        out[CODEX / ".agents" / "skills" / skill / "agents" / "openai.yaml"] = (
            "interface:\n"
            f"  display_name: {q('Skywalker: ' + name.replace('-', ' '))}\n"
            "policy:\n"
            "  allow_implicit_invocation: false\n"
        ).encode()
    server = src.meta["mcp"]
    toml = (
        "# Skywalker MCP server for Codex. Merge into ~/.codex/config.toml (or a trusted project's .codex/config.toml),\n"
        "# or run `skywalker setup codex`, which does this for you with the absolute path of the binary.\n"
        f"[mcp_servers.{server['server']}]\n"
        f"command = {json.dumps(server['command'])}\n"
        f"args = {json.dumps(server['args'])}\n"
        "startup_timeout_sec = 30\n"
        "tool_timeout_sec = 600\n"
    )
    out[CODEX / ".codex" / "config.toml"] = toml.encode()
    for name, a in src.agents.items():
        sid = a["meta"]["studio_id"]
        lines = [f"name = {json.dumps(sid)}", f"description = {json.dumps(a['meta']['description'])}"]
        if a["meta"].get("readonly") == "true":
            lines.append('sandbox_mode = "read-only"')
        lines.append("developer_instructions = " + toml_str(a["body"].rstrip() + "\n"))
        out[CODEX / ".codex" / "agents" / f"{sid}.toml"] = ("\n".join(lines) + "\n").encode()
    return out


def gen_gemini(src: Source) -> dict[Path, bytes]:
    m = src.meta
    out: dict[Path, bytes] = {}
    server = m["mcp"]
    manifest = {
        "name": m["name"],
        "version": m["version"],
        "description": m["description"],
        "contextFileName": "GEMINI.md",
        "mcpServers": {server["server"]: {"command": server["command"], "args": server["args"], "timeout": 600000}},
    }
    out[GEMINI / "gemini-extension.json"] = pretty(manifest).encode()
    addendum = (
        "Gemini CLI specifics: skills activate automatically from their descriptions; the studio roles are subagents (`@creative-director`, `@level-designer`, `@environment-artist`, "
        "`@gameplay-programmer`, `@technical-artist`, `@sound-designer`, `@playtester`, `@critic`); workflows are the `/new-game`, `/look-dev`, `/playtest-loop`, `/studio-status`, `/studio-setup` commands."
    )
    out[GEMINI / "GEMINI.md"] = ("# Skywalker game engine\n\n" + src.context_block(addendum)).encode()
    out.update(src.skill_files(GEMINI / "skills"))
    for name, a in src.agents.items():
        head = frontmatter([("name", name), ("description", a["meta"]["description"]), ("kind", "local")])
        out[GEMINI / "agents" / f"{name}.md"] = (head + a["body"].rstrip() + "\n").encode()
    for name, c in src.commands.items():
        prompt = c["body"].replace("$ARGUMENTS", "{{args}}").rstrip() + "\n"
        out[GEMINI / "commands" / f"{name}.toml"] = (
            f"description = {json.dumps(c['meta']['description'])}\nprompt = {toml_str(prompt)}\n"
        ).encode()
    return out


def gen_cursor(src: Source) -> dict[Path, bytes]:
    out: dict[Path, bytes] = {}
    base = CURSOR / ".cursor"
    out[base / "mcp.json"] = pretty({"mcpServers": src.mcp_servers()}).encode()
    addendum = (
        "Cursor specifics: skills live in `.cursor/skills` (invoke with `/skywalker-core`, or let the agent pick them); studio roles are subagents in `.cursor/agents` "
        "(`/level-designer ...`); workflows are the commands in `.cursor/commands`."
    )
    rule_head = (
        "---\n"
        f"description: {q('Skywalker game engine: how to work with the skywalker MCP tools (loop, conventions, skills)')}\n"
        "alwaysApply: true\n"
        "---\n\n"
    )
    out[base / "rules" / "skywalker.mdc"] = (rule_head + src.context.rstrip() + "\n\n" + addendum + "\n").encode()
    wander_rule = (
        "---\n"
        f"description: {q('Editing Wander behavior files (.wander) for the Skywalker engine')}\n"
        'globs: ["**/*.wander"]\n'
        "alwaysApply: false\n"
        "---\n\n"
        "Wander is Skywalker's behavior language. Read it from the engine before writing any: call the `wander_reference` MCP tool (or read the MCP resource `skywalker://docs/WANDER`).\n"
        "Check source with `wander_check` (or `skywalker check FILE.wander`), attach it with `behavior_set`, and verify by stepping the simulation (`sim_control step`, `sim_trace`, `logs`).\n"
        "Keep each behavior's `intent` accurate whenever the code changes. See the `skywalker-wander` skill.\n"
    )
    out[base / "rules" / "skywalker-wander.mdc"] = wander_rule.encode()
    out.update(src.skill_files(base / "skills"))
    for name, a in src.agents.items():
        pairs: list[tuple[str, str | bool]] = [("name", name), ("description", a["meta"]["description"])]
        pairs.append(("readonly", a["meta"].get("readonly") == "true"))
        out[base / "agents" / f"{name}.md"] = (frontmatter(pairs) + a["body"].rstrip() + "\n").encode()
    for name, c in src.commands.items():
        body = c["body"].replace("$ARGUMENTS", "the arguments the user typed after the command")
        out[base / "commands" / f"{name}.md"] = (f"# {name}\n\n{c['meta']['description']}.\n\n" + body.rstrip() + "\n").encode()
    return out


def generate() -> dict[Path, bytes]:
    src = Source()
    out: dict[Path, bytes] = {}
    for fn in (gen_claude, gen_codex, gen_gemini, gen_cursor):
        out.update(fn(src))
    return out


# ---------------------------------------------------------------------------------------------------------


def stale_paths(expected: dict[Path, bytes]) -> list[tuple[str, Path]]:
    problems: list[tuple[str, Path]] = []
    for path, data in sorted(expected.items()):
        if not path.exists():
            problems.append(("missing", path))
        elif path.read_bytes() != data:
            problems.append(("differs", path))
    owned = set(expected)
    for d in OWNED_DIRS:
        if d.exists():
            for p in sorted(d.rglob("*")):
                if p.is_file() and p not in owned:
                    problems.append(("extra", p))
    return problems


def main(argv: list[str]) -> int:
    expected = generate()
    if "--check" in argv:
        problems = stale_paths(expected)
        for kind, path in problems:
            print(f"{kind}: {path.relative_to(REPO)}")
        if problems:
            print("integrations are out of date: run `python3 integrations/generate.py`", file=sys.stderr)
            return 1
        print(f"integrations up to date ({len(expected)} files)")
        return 0
    for kind, path in stale_paths(expected):
        if kind == "extra":
            path.unlink()
    for path, data in expected.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)
    print(f"wrote {len(expected)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
