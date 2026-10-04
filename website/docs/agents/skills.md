# Skills and integrations

Tools give an agent abilities; skills give it judgement. Skywalker ships thirteen `skywalker-*` skills in the open
Agent Skills format (`SKILL.md` with a name and description, progressive disclosure, optional `references/`), eight
studio-role subagents and five workflow commands, generated for Claude Code, Codex, Gemini CLI and Cursor from one
source and installed with [`skywalker setup`](../getting-started/connect-agent.md).

## The skills

| Skill | Covers | Main tools |
|---|---|---|
| `skywalker-core` | The agent loop (orient, act, look, verify), conventions, batching, undo, efficiency rules, the router to the other skills and a tool index by task. Load first. | `engine_info`, `scene_overview`, `batch`, `viewport_capture`, `history` |
| `skywalker-world-building` | Terrain presets, sculpt and paint, foliage, water, placing things on real ground | `terrain_*`, `foliage_add`, `scatter`, `place_on_surface`, `water_query` |
| `skywalker-look-dev` | Lighting recipes (golden hour, overcast, night neon, interior), GI, reflections, clouds, god rays, exposure, looks, LUTs, lens; judging renders with samples and debug views | `environment_update`, `material_create`, `viewport_capture`, `viewport_quality`, `perf_stats` |
| `skywalker-assets` | Reuse, procedural textures, licensed downloads with credits, import settings | `asset_list`, `asset_download`, `asset_import`, `texture_generate` |
| `skywalker-dcc` | Blender bridge: procedural models, conversion, decimation and baking, live sessions | `dcc_generate`, `dcc_convert`, `dcc_edit_asset`, `dcc_session_start` |
| `skywalker-studio` | Roster, board, feedback, director decisions with measured effects, playtest bots, loops, joining as a roster member | `studio_*`, `playtest_run` |
| `skywalker-audio` | Generating and measuring effects and music, spatial audio and the mixer, input action maps | `audio_generate`, `audio_info`, `audio_mix`, `input_map` |
| `skywalker-wander` | Behavior workflow: reference, spec, code with tests, check, attach, test, trace | `wander_reference`, `wander_check`, `behavior_set`, `wander_test`, `behavior_spec` |
| `skywalker-physics` | Bodies, colliders, characters, triggers, joints, queries, settling, navigation | `physics_add`, `physics_query`, `physics_settle`, `nav_build` |
| `skywalker-2d-ui` | Sprites, tilemaps, 2D lights, parallax, HUDs and menus, dialogue | `sprite_sheet_slice`, `tilemap_from_ascii`, `ui_create`, `dialogue_check` |
| `skywalker-animation` | Rigged characters, state machines, IK, props on bones, cutscenes and camera shots | `animator_setup`, `animator_set`, `bone_attach`, `sequence_camera_shot` |
| `skywalker-vfx` | Fire, smoke, weather, GPU particles, volumetric fluids, hair and fur, effect cost | `fx_create`, `fx_burst`, `groom_create`, `fx_benchmark` |
| `skywalker-ship` | game.json, packaging checks, building the app, trying it in the real player | `game_settings`, `game_build`, `game_run` |

Every skill is also an MCP resource (`skywalker://skills/<name>`), so clients without skill support can read them.

## Subagents and commands

The eight subagents map to studio roles; each first calls `studio_agent_brief {agent, loop_member: true}` for the
engine's own mission, persona, team and permitted tools, works as that roster member (`as: "<id>"`), verifies with
captures, traces or playtests, and finishes with a short report.

| Studio id | Claude Code | Codex | Gemini CLI | Cursor |
|---|---|---|---|---|
| `creative_director` | `skywalker:creative-director` | `creative_director` | `@creative-director` | `/creative-director` |
| `level_designer` | `skywalker:level-designer` | `level_designer` | `@level-designer` | `/level-designer` |
| `environment_artist` | `skywalker:environment-artist` | `environment_artist` | `@environment-artist` | `/environment-artist` |
| `gameplay_programmer` | `skywalker:gameplay-programmer` | `gameplay_programmer` | `@gameplay-programmer` | `/gameplay-programmer` |
| `technical_artist` | `skywalker:technical-artist` | `technical_artist` | `@technical-artist` | `/technical-artist` |
| `sound_designer` | `skywalker:sound-designer` | `sound_designer` | `@sound-designer` | `/sound-designer` |
| `playtester` | `skywalker:playtester` | `playtester` | `@playtester` | `/playtester` |
| `critic` | `skywalker:critic` | `critic` | `@critic` | `/critic` |

| Command | What it does |
|---|---|
| `new-game` | Build a new game from a one-line pitch |
| `look-dev` | Art-direct the current scene with captures and debug views |
| `playtest-loop` | Run a studio loop: start, one subagent per assignment, advance until done |
| `studio-status` | Summarise the roster, board, feedback and loops |
| `studio-setup` | Create the roster with `studio_team_template` |

## Where the files go

```text
integrations/skills-src/   the single source: skills, subagents, commands, project guidance
        │  python3 integrations/generate.py
        ▼
integrations/claude-code/  Claude Code plugin (plus .claude-plugin/marketplace.json at the repository root)
integrations/codex/        AGENTS.md, .agents/skills, .codex/{config.toml, agents}
integrations/gemini/       Gemini CLI extension
integrations/cursor/       .cursor/{mcp.json, rules, skills, agents, commands}
```

Skills are byte-identical across clients; only the wrappers (manifests, subagent files, commands, MCP config) differ.

## Keeping skills true

Every backticked tool name, argument and enum value in the skill sources, and every example call such as
`physics_add {"entity": "Hero", "preset": "player_character"}`, is checked against the live tool schemas. The test
suite runs the check, so a renamed tool or argument that would break a skill fails the build:

```bash
skywalker tools --json > /tmp/tools.json
python3 integrations/check_skills.py --tools /tmp/tools.json
python3 integrations/generate.py --check
```

This site applies the same validation to every sample on every page (`website/scripts/check_snippets.py`).

!!! agent "For agents"

    Load `skywalker-core` first, then the specialist skill **before** the first call in its area: each has the
    workflow, exact example calls and the pitfalls. Without skill support, read the resource:

    ```text
    resources/read {"uri": "skywalker://skills/skywalker-look-dev"}
    ```
