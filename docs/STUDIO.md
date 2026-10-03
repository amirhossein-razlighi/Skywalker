# The Studio

The Studio is a game studio of AI agents (and humans) that lives inside the project. It has
a roster of specialists, a task board, feedback from playtesters and critics, a director who
decides what to do with that feedback, loops that repeat play → triage → fix → verify, message
threads, and per-agent budgets. Bots play the game for real and measure it.

It lives engine-side and is reached through tools, so every client shares the same studio:
the editor's crew, the headless runner (`skywalker studio run`), and external agents such as
Claude Code, Codex, Gemini CLI and Cursor over MCP.

```
                ┌──────────── studio tools (studio_*, playtest_*) ────────────┐
 editor crew ──►│ roster · board · feedback · decisions · loops · messages    │◄── Claude Code / Codex
 CLI runner  ──►│ usage · playtests (sandbox engine + bots)                   │◄── any MCP client
                └────────────── project files (git-friendly) ─────────────────┘
```

## Quick start

From any MCP client (Claude Code shown):

```text
studio_team_template {template: "aaa_strike_team"}          # 22 specialists
studio_loop_define   {template: "playtest_fix_verify", goal: "Players finish the canyon run without unfair deaths"}
studio_loop_start    {loop: "playtest_fix_verify"}           # returns assignments with prompts
… run each assignment as that agent, then:
studio_loop_advance  {loop: "playtest_fix_verify", reports: [{agent: "nimbus", report: "…"}]}
```

Headless (CI, overnight):

```bash
export ANTHROPIC_API_KEY=…                       # keys come from the environment only
skywalker studio run --project MyGame --loop playtest_fix_verify --iterations 3
skywalker studio feedback --project MyGame       # verdicts and measured effects
```

In the editor: **Studio** dock tab → Loops → *Run with Crew*.

## Project files

| File | Contents |
|---|---|
| `agents/<id>.agent.json` | One agent profile per file (the same files the editor's crew uses). |
| `studio/board.json` | Tasks. |
| `studio/feedback.json` | Feedback items with history. |
| `studio/decisions.json` | Director/producer verdicts and their measured effect. |
| `studio/loops/<name>.loop.json` | Loop definition plus runtime state (iterations, stage reports, trends). |
| `studio/messages.jsonl` | Messages, one per line (append-only). |
| `studio/usage.json` | Tokens, estimated cost and tool calls per agent; inbox read cursors; latest playtest metrics. |
| `studio/playtests/<P-n>/` | `report.json`, `heatmap.png`, `shot_*.png`. |

Everything is pretty-printed JSON with stable key order, written atomically, and re-read when
another process (the CLI, git, a teammate) changes it. Ids are short and readable: `T-12`,
`F-7`, `D-3`, `M-31`, `P-4`.

## Roster

An agent profile:

| Field | Meaning |
|---|---|
| `id`, `name` | `id` is the file name and the @mention handle. |
| `discipline` | direction, production, design, engineering, art, audio, writing, qa |
| `role` | creative_director, art_director, producer, systems_designer, level_designer, narrative_designer, economy_designer, ux_designer, gameplay_programmer, ai_programmer, graphics_programmer, tools_programmer, environment_artist, lighting_artist, character_artist, vfx_artist, technical_artist, ui_artist, sound_designer, composer, writer, qa_lead, playtester, critic |
| `focus`, `focus_tags` | What exactly it owns ("enemy AI", "onboarding", "secret areas"). Several agents can share a role with different focuses. |
| `persona`, `mission`, `instructions` | Voice; a replacement for the role's mission; standing instructions. |
| `provider`, `model` | `anthropic` (default model `claude-opus-5-5`), `openai`, any OpenAI-compatible provider name, or `mock`. |
| `autonomy` | `observe` (read-only), `ask` (changes need a human), `autonomous`. |
| `permissions` | Per tool category: `allow`, `ask`, `off`. `off` removes the tools entirely. |
| `reports_to` | Its lead. |
| `color`, `face` | Avatar. |
| `memory` | Long-term notes (`studio_memory`). |
| `playtest` | Playtester persona: `{policy, reaction_time, skill, curiosity, patience}`. |

Studio coordination tools (category `studio`: board, feedback, messages, playtests) are
available at every autonomy level unless explicitly turned off — they never change the game.
Downloads (`network`) always ask unless explicitly allowed. Only `direction` and `production`
agents (or humans) may decide on feedback.

Team templates (`studio_team_template`): `starter_crew`, `indie_trio`, `aaa_strike_team`,
`narrative_team`, `qa_squad`, `art_team`, `audio_team`.

## Board, feedback, decisions

- **Tasks** have a title, description, acceptance criteria, discipline, assignee, status
  (`backlog → todo → doing → review → done`, or `dropped`), priority, dependencies (moving to
  doing/done checks them), links (feedback, entities, assets, captures, playtests) and a
  comment thread. `studio_task_claim` without a task picks the caller's next task.
- **Feedback** has a category (fun, difficulty, clarity, visuals, audio, performance, bug,
  narrative, accessibility), severity, summary, details, target, evidence (`playtest`,
  `captures`, `metrics`, `positions`, `repro`, `trace`) and a status: `open → accepted →
  in_progress → fixed → verified`, or `dropped`, `deferred`, `regressed`. A `fingerprint`
  deduplicates recurring findings (a repeat bumps `occurrences`).
- **Decisions** (`studio_decide`): `act` (creates tasks linked to the feedback), `drop`,
  `defer`, or `merge_into` a duplicate — always with a rationale everyone sees. The latest
  playtest metrics are stored as the baseline (`metrics_before`), optionally with explicit
  `targets` (`{"deaths": {"max": 1}}`).
- **Effect.** When the last task of an accepted item is done, the item becomes `fixed`. The
  next verification playtest (a loop stage with `verify_fixed`, or `playtest_compare
  {feedback}`) compares before/after: `improved` → `verified`; `regressed` → `regressed` and
  its tasks reopen; `unchanged`/`mixed` stay `fixed` for a human; categories without metrics
  (visuals, audio, narrative) are `unmeasured` until a critic or human verifies them with
  `studio_feedback_update`. Metrics are direction-aware (fewer deaths, higher completion are
  better) with a 5% tolerance and a noise floor for timings.

Metrics that measure each category: difficulty → deaths, completion_rate, time_to_goal,
fails, damage; clarity → stuck_seconds, completion_rate, time_to_goal; performance → est_fps,
avg/p95 tick ms; bug → script_errors, fails, stuck_seconds; fun → completion_rate,
quit_rate, coverage.

## Loops

A loop is data: stages, stop conditions and runtime state.

```json
{
  "name": "playtest_fix_verify",
  "goal": "Make the level fun and fair to complete.",
  "stages": [
    {"id": "playtest", "kind": "playtest", "playtest": {"policy": "goal_seeker", "runs": 3, "seconds": 45},
     "gate": {"skip_if": "not_first_iteration"}},
    {"id": "play", "assignees": ["@playtester"], "instruction": "Play with your persona… {{inputs}}"},
    {"id": "triage", "assignees": ["@creative_director|@producer"], "gate": {"skip_if": "no_open_feedback"},
     "instruction": "Triage every open item with studio_decide…\n{{open_feedback}}\nMetrics: {{metrics}}"},
    {"id": "fix", "assignees": ["@design", "@engineering", "@art"], "only_with_tasks": true,
     "instruction": "Your tasks:\n{{my_tasks}}"},
    {"id": "verify", "kind": "playtest", "playtest": {"runs": 3, "seconds": 45}, "verify_fixed": true}
  ],
  "stop": {"max_iterations": 3, "metric_targets": {"completion_rate": {"min": 1}},
           "director_signoff": false, "token_budget": 0, "time_budget_minutes": 0}
}
```

- **Stage kinds.** `playtest` stages run inside the engine (no LLM): they play, file
  findings as feedback (`file_feedback`, deduplicated), and with `verify_fixed` measure the
  effect of fixed items. `agents` stages produce assignments.
- **Assignees.** `@role`, `@discipline`, `@all`, or an agent id; `"@a|@b"` takes the first
  alternative that matches anyone. `only_with_tasks` gives work only to agents holding open
  tasks (unassigned tasks of a matching discipline are auto-assigned to the least busy one).
  `parallel` (default true) lets the executor run a stage's agents at the same time.
- **Instruction variables.** `{{goal}} {{loop}} {{iteration}} {{max_iterations}} {{stage}}
  {{agent}} {{role}} {{focus}} {{inputs}}` (reports of earlier stages this iteration, or of
  `inputs: [stage ids]`) `{{previous}} {{open_feedback}} {{fixed_feedback}} {{my_tasks}}
  {{todo_tasks}} {{metrics}} {{trends}} {{roster}}`.
- **Gates.** `skip_if`: `no_open_feedback`, `no_todo_tasks`, `no_fixed_feedback`,
  `first_iteration`, `not_first_iteration` (one or a list). `approval: "human"` pauses the
  loop until `studio_loop_advance {approve: true}`.
- **Stop conditions.** Success conditions — `metric_targets` (on the iteration's last
  playtest) and `director_signoff` (a direction/production agent reports `SIGNOFF` or passes
  `signoff: true`) — must all hold. Limits — `max_iterations`, `token_budget` (all studio
  tokens since the run started), `time_budget_minutes` — end the run.
- **State** (`studio_loop_status`): status (`idle`, `running`, `awaiting_approval`, `done`,
  `stopped`), iteration, stage, pending assignments with full prompts, every iteration's
  stage records (reports, playtest ids and metrics, filed and verified feedback), metric
  trends, tokens used, and a summary of previous runs.

Templates: `playtest_fix_verify`, `art_pass_with_critic`, `balance_tuning`,
`vertical_slice_sprint`, `bug_bash`.

**Who executes agent stages?** Whoever drives the loop: the loop is a state machine behind
`studio_loop_start` / `studio_loop_advance`. The headless runner and the editor's crew both
use exactly these tools; so can an external agent (below).

## Playtests that play

`playtest_run` copies the current scene into a sandbox engine (the scene you edit, its undo
history and selection are never touched), presses play and drives the game for N seconds
with a bot through the same `sim_input` tool agents use.

| Policy | Behavior |
|---|---|
| `goal_seeker` | Walks to the nearest unreached entity tagged `goal`/`objective`, steers around `hazard`/`enemy`/`danger`, slides along walls (ray casts), jumps over low obstacles, detours when stuck, quits when it makes no progress for `patience` seconds. Explores if the level has no goals. |
| `explorer` | Covers unexplored 2 m cells of the level, avoiding hazards. |
| `random` | Random headings and jumps. |
| `scripted` | `script: [{t, hold/release/press/click/event…}]` — any `sim_input` arguments at given times. |

Persona: `reaction_time` (seconds between decisions), `skill` (steering precision, hazard
clearance, jumping), `curiosity` (detours toward unexplored space), `patience`. Playtester
agents play with their own persona by default. `runs` aggregates several seeds.

**Game conventions the bots understand.** Tag the player `player`, goals `goal`/`objective`,
dangers `hazard`/`enemy`. Emit events from Wander: `death`/`died`/`killed`/`respawn` →
deaths; `fail`/`lose`/`game_over` → fails; `damage`/`hurt`/`hit` → damage;
`objective`/`goal`/`win`/`level_complete`/`victory` → objectives; `*collected`/`pickup` →
pickups; `checkpoint`. A sudden teleport of the player without a death event counts as a
respawn death. Movement keys default to WASD in world space (`w` = −Z) and `space` to jump;
override with `controls`. (When input actions land, bots keep driving `sim_input`.)

**Report.** Metrics (`completion_rate`, `time_to_goal`, `deaths`, `fails`, `damage`,
`objectives`, `stuck_seconds`, `coverage`, `distance`, `quit_rate`, `script_errors`,
`avg_tick_ms`, `p95_tick_ms`, `est_fps`), death causes (nearest hazard), per-run
trajectories, events, stuck periods and objectives, screenshots at notable moments (death,
goal, stuck, quit, end; plus `screenshot_interval`), a top-down heatmap (geometry, hazards in
red, goals in green, trajectory density, deaths ×, stuck ○) and **findings** — the feedback a
QA lead would file, with evidence and fingerprints. Frame cost samples a 960×540 render
every 2 s after a warm-up frame.

Simulation is deterministic: the same scene and seed give the same report (timings aside).
`playtest_compare {before, after, feedback?}` diffs two reports and can record the effect.

## Tools

| Tool | Purpose |
|---|---|
| `studio_overview` | Roster with live status, board and feedback counts, items needing a decision, recent verdicts, loops, latest metrics, usage. `include_catalog` lists roles and templates. |
| `studio_agent_define` / `_list` / `_remove` | Create/update (merge), list, remove agents. |
| `studio_agent_brief` | An agent's system prompt and permitted tools — run it in any harness. |
| `studio_team_template` | Spawn a team. |
| `studio_memory` | An agent's long-term notes. |
| `studio_usage_report` | Report tokens spent outside the engine. |
| `studio_task_create` / `_update` / `_list` / `_claim` | The board. |
| `studio_feedback_submit` / `_list` / `_update` | Feedback (lists include verdicts and effects). |
| `studio_decide` | Director/producer verdicts. |
| `studio_loop_define` / `_start` / `_advance` / `_status` / `_stop` | Loops. |
| `studio_message_send` / `studio_inbox` | Channels, threads, @mentions. |
| `playtest_run` / `playtest_compare` | Playtests and before/after comparison. |

## Identity: how external tools join

Every tool call carries an actor. Studio tools map it to a roster member:

- `agent:<id>` — agents run by the editor crew or the headless runner;
- `mcp:<client>/<id>` — an MCP client whose `clientInfo.name` ends in `/<agent id>` (e.g.
  `skywalker call … --attach --as claude-code/mira`);
- `as: "<id>"` on studio calls — for clients that can't rename themselves (each Claude Code
  subagent shares the session's MCP connection). An identified member cannot act as another.

Unidentified callers (`user`, `mcp:claude-code`) act for the human: they can decide, sign
off and approve.

**Running a loop from Claude Code (or Codex, Gemini CLI, Cursor).**

1. `studio_loop_start {loop}` → `assignments: [{agent, stage, prompt, tasks}]`, `parallel`.
2. For each assignment, `studio_agent_brief {agent, loop_member: true}` gives the system
   prompt and the tools that agent may use. Spawn a subagent with that prompt and the
   assignment's `prompt`; it passes `as: "<agent id>"` on studio calls.
3. `studio_loop_advance {loop, reports: [{agent, report, usage?}]}` — reports may arrive one
   at a time; the loop moves on when all pending agents reported (or `complete_stage`).
4. Repeat until `status` is `done`, `stopped` or `awaiting_approval`.

## Headless runner

```text
skywalker studio status|agents|board|feedback|loops --project DIR
skywalker studio run --project DIR --loop NAME [--iterations N] [--goal TEXT]
                     [--dry-run | --mock SCRIPT.json] [--yes] [--scene FILE] [--no-save] [--verbose]
```

- Loads `--scene` or `scenes/main.sky.json`; saves it at the end unless `--no-save` or a dry
  run. A template name defines the loop on first use.
- **Providers** per agent, keys from the environment only (never written to disk):
  `anthropic` → `ANTHROPIC_API_KEY` (`ANTHROPIC_BASE_URL`); anything else is
  OpenAI-compatible → `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_MODEL`, `OPENAI_VISION=0`
  for text-only models; local servers need no key.
- **Anthropic Messages API** (raw HTTPS over libcurl): adaptive thinking with
  `output_config.effort: "high"` (on models that support it), prompt caching (a breakpoint on
  the system block covers tools + system; top-level automatic caching follows the history),
  server-side refusal fallback (`fallbacks: "default"`) on supported models, append-only
  history (thinking blocks echoed unchanged), tool results with screenshots as image blocks,
  `stop_reason` handling: `tool_use`, `end_turn`, `max_tokens` (a cut-off tool call is never
  run — the model is told to re-issue it), `refusal` (partial output discarded, tools never
  run), `pause_turn` (resumed). Retries 429/5xx/529 with backoff.
- **OpenAI-compatible Chat Completions** with function tools; screenshots follow tool
  messages as an image user message; invalid tool-argument JSON is returned as an
  `INVALID_JSON` error.
- Agents get only the tools their permissions allow; `ask` tools are declined unless `--yes`
  (headless runs have no human). Inside loops, loop and roster control tools are withheld.
- Parallel stages run on worker threads; all engine access goes through `Engine::post`.
- `--dry-run` uses a stub model (no API calls, no cost) and prints each agent's full prompt;
  playtest stages still run. `--mock` takes a scripted conversation:
  `{"agents": {"<id>": [{"text": "…", "tool_calls": [{"name": "…", "input": {…}}], "stop": "end_turn|max_tokens|refusal|pause_turn"}]}, "default": [...]}`.
- Ctrl-C stops the loop cleanly (`studio_loop_stop`).

## Editor

The **Studio** dock tab has five views: **Roster** (avatar, role, focus, live status and
activity, open tasks, tokens and cost; spawn teams, add by role, design), **Board** (Kanban
with drag-and-drop between columns, priorities, linked feedback, assignee avatars, comments),
**Feedback** (each item with its verdict, rationale and measured effect as metric deltas;
decide from the UI), **Loops** (iterations × stages timeline with live progress, playtest
results, filed and verified feedback, a metric sparkline, run/stop/approve), and
**Messages** (channels and threads). The crew (Agents tab) is the same roster: chats use the
engine's agent brief (system prompt and permitted tools), report usage to the studio, and
*Run with Crew* drives loops through the same tools as the CLI.

## Implementation

| Path | Contents |
|---|---|
| `engine/include/skywalker/studio/Studio.h`, `src/studio/Studio.cpp` | Records, persistence, roster, board, feedback, decisions, effects, loop state machine, messages, usage. |
| `studio/Catalog.h`, `src/studio/Catalog.cpp` | Roles, team templates, loop templates. |
| `studio/Playtest.h`, `src/studio/Playtest.cpp` | Sandbox playtests, bots, heatmaps, findings. |
| `studio/Llm.h`, `src/studio/Llm.cpp` | Anthropic, OpenAI-compatible and mock providers over libcurl. |
| `studio/Runner.h`, `src/studio/Runner.cpp` | Agent runner, loop runner. |
| `engine/src/agent/StudioTools.cpp` | The tools. |
| `tools/cli/StudioCommand.cpp` | `skywalker studio`. |
| `editor/Sources/Studio/` | StudioStore and the Studio panel. |

The studio is main-thread state like the rest of the engine, created on first use
(`Engine::studio()`). A Wander hook (`Runtime::onEmit`) lets playtests observe events.

## Limitations

- Bots steer without a navigation mesh (ray casts and potential fields); mazes defeat them.
  They will use navmesh paths when navigation lands.
- Playtests run synchronously on the engine thread: in the editor, a long playtest blocks the
  UI while it runs.
- Movement mapping assumes world-space WASD unless `controls` says otherwise; camera-relative
  schemes need a scripted policy for now.
- Token budgets count tokens the studio knows about (the runner, the editor crew, and
  `studio_usage_report` / `studio_loop_advance` usage from external executors).
