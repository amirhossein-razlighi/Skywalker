#!/usr/bin/env python3
"""Lint the skill sources against the engine's real tool schemas.

    skywalker tools --json > tools.json
    python3 integrations/check_skills.py --tools tools.json

Checks (all on integrations/skills-src):
  1. every `backticked_tool_name` that looks like an engine tool exists (tool names, argument names and enum values count as known words);
  2. every example call in a fenced block, `tool_name {arg: value, ...}` (JS-style keys allowed), parses, uses only
     arguments the tool defines, passes enum values the schema allows, and has the right basic types and required arguments
     (`batch` operations are checked recursively).
Exit status 1 lists every problem. Standard library only.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "skills-src"

# Words that look like tool names but are not (recipe names, file paths, wander functions...).
ALLOW = {
    "terrain_chunk", "wind_loop", "rain_loop", "fire_crackle_loop", "ocean_waves_loop", "ambient_drone", "footstep_grass", "footstep_stone",
    "footstep_wood", "ui_click", "ui_hover", "play_sound", "stop_sound", "set_volume", "water_height", "tool_timeout_sec", "startup_timeout_sec",
    "studio_id", "scene_query", "place_on_surface", "sim_trace", "audio_info", "asset_list", "playtest_run", "perf_stats", "select_unit",
    "camera_pan", "game_over", "level_complete", "per_surface", "focus_tags", "reports_to", "reaction_time", "file_feedback", "verify_fixed",
    "only_with_tasks", "metric_targets", "director_signoff", "token_budget", "time_budget_minutes", "max_iterations", "skip_if",
    "no_open_feedback", "no_todo_tasks", "no_fixed_feedback", "first_iteration", "not_first_iteration", "needs_decision", "in_progress",
    "loop_member", "include_catalog", "include_profiles", "unread_only", "list_cmds", "mcp_servers", "enabled_tools", "disabled_tools",
    "max_entities", "include_image", "save_path", "focus_distance", "camera_entity", "debug_view", "godRays", "scripts_dir",
    "aaa_strike_team", "starter_crew", "indie_trio", "narrative_team", "qa_squad", "art_team", "audio_team", "playtest_fix_verify",
    "art_pass_with_critic", "balance_tuning", "vertical_slice_sprint", "bug_bash", "island_beach", "tropical_coast", "mountain_valley",
    "rolling_hills", "desert_dunes", "meadow_grass", "tall_grass", "dune_grass", "beach_pebbles", "rocks_small", "calm_sea", "debris_smoke",
    "volume_fire", "volume_torch", "volume_smoke", "steam_vent", "explosion_volume", "sprite_campfire", "burning_barrel", "brushed_steel",
    "car_paint", "toon_metal", "metal_brushed", "teal_orange", "golden_hour", "creative_director", "art_director", "systems_designer",
    "level_designer", "narrative_designer", "economy_designer", "ux_designer", "gameplay_programmer", "ai_programmer", "graphics_programmer",
    "tools_programmer", "environment_artist", "lighting_artist", "character_artist", "vfx_artist", "technical_artist", "ui_artist",
    "sound_designer", "qa_lead", "role_creative_director", "new_game", "look_dev", "playtest_loop", "studio_status", "studio_setup",
    "studio_agent", "studio_overview", "studio_roster", "studio_board", "studio_feedback", "studio_loops", "ask_for_approval",
}
FAMILIES = {
    "entity", "scene", "terrain", "studio", "dcc", "asset", "audio", "sim", "viewport", "camera", "selection", "material", "texture",
    "prefab", "fx", "foliage", "playtest", "behavior", "wander", "environment", "shader", "perf", "input", "place", "water", "component",
}


def load_tools(path: Path) -> dict[str, dict]:
    data = json.loads(path.read_text())
    return {t["name"]: t for t in (data["tools"] if isinstance(data, dict) else data)}


def known_words(tools: dict[str, dict]) -> set[str]:
    words = set(tools)
    def walk(schema):
        if isinstance(schema, dict):
            for key, value in schema.get("properties", {}).items():
                words.add(key)
                walk(value)
            for e in schema.get("enum", []) or []:
                if isinstance(e, str):
                    words.add(e)
            if "items" in schema:
                walk(schema["items"])
        elif isinstance(schema, list):
            for s in schema:
                walk(s)
    for t in tools.values():
        walk(t["inputSchema"])
    return words


def strip_comments(text: str) -> str:
    out, i, in_str, quote = [], 0, False, ""
    while i < len(text):
        c = text[i]
        if in_str:
            out.append("\\n" if c == "\n" else c)
            if c == "\\" and i + 1 < len(text):
                out.append(text[i + 1]); i += 1
            elif c == quote:
                in_str = False
        elif c in "\"'":
            in_str, quote = True, c
            out.append(c)
        elif c == "#" and (i == 0 or text[i - 1] in " \t\n"):
            while i < len(text) and text[i] != "\n":
                i += 1
            continue
        else:
            out.append(c)
        i += 1
    return "".join(out)


def js_to_json(text: str) -> str:
    # quote bare keys: {preset: 1, "x": 2} -> {"preset": 1, "x": 2}; keys are identifiers followed by a colon after { or ,
    return re.sub(r'([{,]\s*)([A-Za-z_][A-Za-z0-9_]*)(\s*:)', r'\1"\2"\3', text)


def balanced(text: str, start: int) -> int:
    depth, i, in_str, quote = 0, start, False, ""
    while i < len(text):
        c = text[i]
        if in_str:
            if c == "\\":
                i += 1
            elif c == quote:
                in_str = False
        elif c in "\"'":
            in_str, quote = True, c
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return -1


def check_value(value, schema: dict, where: str, problems: list[str]) -> None:
    if not isinstance(schema, dict):
        return
    t = schema.get("type")
    types = t if isinstance(t, list) else [t] if t else []
    checks = {"string": str, "integer": int, "number": (int, float), "boolean": bool, "array": list, "object": dict}
    if types and not any(isinstance(value, checks[x]) and not (x in ("integer", "number") and isinstance(value, bool)) for x in types if x in checks):
        problems.append(f"{where}: expected {'/'.join(types)}, got {json.dumps(value)[:40]}")
        return
    if "enum" in schema and value not in schema["enum"]:
        problems.append(f"{where}: {value!r} is not one of {schema['enum']}")
    if isinstance(value, dict) and "properties" in schema:
        check_args(value, schema, where, problems)
    if isinstance(value, list) and isinstance(schema.get("items"), dict):
        for i, item in enumerate(value):
            check_value(item, schema["items"], f"{where}[{i}]", problems)


def check_args(args: dict, schema: dict, where: str, problems: list[str]) -> None:
    props = schema.get("properties", {})
    for key in schema.get("required", []):
        if key not in args:
            problems.append(f"{where}: missing required argument '{key}'")
    for key, value in args.items():
        if key not in props:
            if schema.get("additionalProperties") is False:
                problems.append(f"{where}: unknown argument '{key}' (known: {', '.join(props)})")
            continue
        check_value(value, props[key], f"{where}.{key}", problems)


def check_call(name: str, args, tools: dict[str, dict], where: str, problems: list[str]) -> None:
    if not isinstance(args, dict):
        problems.append(f"{where}: arguments of {name} are not an object")
        return
    check_args(args, tools[name]["inputSchema"], f"{where} {name}", problems)
    if name == "batch":
        for i, op in enumerate(args.get("operations", [])):
            tool = op.get("tool")
            if tool not in tools:
                problems.append(f"{where} batch[{i}]: unknown tool {tool!r}")
            else:
                check_call(tool, op.get("args", {}), tools, f"{where} batch[{i}]", problems)


def fenced_blocks(text: str):
    for m in re.finditer(r"```([A-Za-z0-9_-]*)\n(.*?)```", text, re.S):
        yield m.group(1), m.group(2), text[: m.start()].count("\n") + 1


checked = [0]


def check_file(path: Path, tools: dict[str, dict], words: set[str], problems: list[str]) -> None:
    text = path.read_text()
    rel = path.relative_to(HERE)
    in_fence = [(m.start(), m.end()) for m in re.finditer(r"```.*?```", text, re.S)]
    for m in re.finditer(r"`([^`\n]+)`", text):
        if any(a <= m.start() < b for a, b in in_fence):
            continue
        token = re.split(r"[\s{(]", m.group(1))[0].rstrip(",.;:")
        if re.fullmatch(r"[a-z]+(_[a-z0-9]+)+", token) and token.split("_")[0] in FAMILIES and token not in words and token not in ALLOW:
            line = text[: m.start()].count("\n") + 1
            problems.append(f"{rel}:{line}: `{token}` looks like an engine tool but no such tool/argument exists")
    for lang, block, first_line in fenced_blocks(text):
        if lang not in ("", "text"):
            continue
        for m in re.finditer(r"(?<![A-Za-z0-9_])([a-z][a-z0-9_]*)\s*\{", block):
            name = m.group(1)
            if name not in tools:
                continue
            end = balanced(block, m.end() - 1)
            line = first_line + block[: m.start()].count("\n") + 1
            where = f"{rel}:{line}"
            if end < 0:
                problems.append(f"{where}: unbalanced braces in the {name} example")
                continue
            raw = block[m.end() - 1: end]
            try:
                args = json.loads(js_to_json(strip_comments(raw)))
            except json.JSONDecodeError as e:
                problems.append(f"{where}: {name} example is not valid JSON-ish ({e.msg} at {e.pos}): {raw[:60]!r}")
                continue
            check_call(name, args, tools, where, problems)
            checked[0] += 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tools", required=True, help="output of `skywalker tools --json`")
    opts = ap.parse_args()
    tools = load_tools(Path(opts.tools))
    words = known_words(tools)
    problems: list[str] = []
    files = sorted(p for p in SRC.rglob("*.md"))
    for p in files:
        check_file(p, tools, words, problems)
    for line in problems:
        print(line)
    if problems:
        print(f"{len(problems)} problem(s) in skill sources", file=sys.stderr)
        return 1
    print(f"skill sources consistent with {len(tools)} engine tools ({len(files)} files, {checked[0]} example calls checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
