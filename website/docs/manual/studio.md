# Studio and crews

The studio is a game studio of AI agents, and the people who work with them, that lives inside your project. It has a
roster of specialists, a task board, feedback from playtesters and critics, a director who decides what to do with
that feedback, loops that repeat play, triage, fix and verify, message threads and per-agent budgets. Playtest bots
play the game for real and measure it, so a fix is judged by numbers rather than by opinion.

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/studio-roster.webp · The Studio dock, Roster view: agent cards with avatar, role, focus, live status, open tasks, tokens and cost</div>

## Concepts

### One studio, many clients

The studio lives in the engine and in the project's files, and it is reached only through tools. Every client sees and
changes the same studio:

| Client | How it drives the studio |
|---|---|
| The editor's crew | Chats with agents in the Agents tab; **Run with Crew** drives loops from the Studio panel. |
| The headless runner | `skywalker studio run` executes a loop with your API keys, for CI or overnight runs. |
| External agents | Claude Code, Codex, Gemini CLI, Cursor or any MCP client call the `studio_*` and `playtest_*` tools. |
| Python agents | The [Python agent layer](../agents/python.md) runs roster members with workflows, shared memory and approvals; they appear in the Studio panel with their presence (`studio_presence`) and attributed edits. |

The editor, the runner and external agents use exactly the same tools, so an agent behaves the same wherever it runs:
its system prompt and permitted tools come from the engine (`studio_agent_brief`).

### Project files

Everything is pretty-printed JSON with a stable key order, written atomically and re-read when another process (the
CLI, git, a teammate) changes it. Commit it and the whole team shares the studio.

| File | Contents |
|---|---|
| `agents/<id>.agent.json` | One agent profile per file (the same files the editor's crew uses) |
| `studio/board.json` | Tasks |
| `studio/feedback.json` | Feedback items with their history |
| `studio/decisions.json` | Director and producer verdicts with their measured effect |
| `studio/loops/<name>.loop.json` | Loop definition plus runtime state (iterations, stage reports, trends) |
| `studio/messages.jsonl` | Messages, one per line, append-only |
| `studio/usage.json` | Tokens, estimated cost and tool calls per agent; inbox read cursors; latest playtest metrics |
| `studio/playtests/<P-n>/` | `report.json`, `heatmap.png`, `shot_*.png` |

Ids are short and readable: `T-12` (task), `F-7` (feedback), `D-3` (decision), `M-31` (message), `P-4` (playtest).
API keys are never written to these files. `skywalker build` never packages `studio/` or `agents/`.

### The roster

An agent profile:

| Field | Meaning |
|---|---|
| `id`, `name` | `id` is the file name and the @mention handle. |
| `discipline` | `direction`, `production`, `design`, `engineering`, `art`, `audio`, `writing`, `qa` |
| `role` | One of 24 roles (below). The role's mission becomes part of the system prompt. |
| `focus`, `focus_tags` | What exactly it owns ("enemy AI", "onboarding", "secret areas"). Several agents can share a role with different focuses. |
| `persona`, `mission`, `instructions` | Voice; a replacement for the role's mission; standing instructions such as style guides and constraints. |
| `provider`, `model` | `anthropic` (default model `claude-opus-5-5`), `openai`, any OpenAI-compatible provider name, or `mock`. |
| `autonomy` | `observe` (read-only tools), `ask` (changes need a human), `autonomous`. |
| `permissions` | Per tool category: `allow`, `ask` or `off`. `off` removes the tools from what the model is offered. |
| `reports_to` | Its lead's id. |
| `color`, `face` | The avatar. |
| `memory` | Long-term notes, kept with `studio_memory` and shown in the system prompt. |
| `max_rounds` | Tool rounds per task (default 40). |
| `playtest` | A playtester persona: `{policy, reaction_time, skill, curiosity, patience}`. |

| Discipline | Roles |
|---|---|
| direction | `creative_director`, `art_director` |
| production | `producer` |
| design | `systems_designer`, `level_designer`, `narrative_designer`, `economy_designer`, `ux_designer` |
| engineering | `gameplay_programmer`, `ai_programmer`, `graphics_programmer`, `tools_programmer` |
| art | `environment_artist`, `lighting_artist`, `character_artist`, `vfx_artist`, `technical_artist`, `ui_artist` |
| audio | `sound_designer`, `composer` |
| writing | `writer` |
| qa | `qa_lead`, `playtester`, `critic` |

**Permissions.** Studio coordination tools (category `studio`: board, feedback, messages, playtests) are available at
every autonomy level unless explicitly turned off; they never change the game. Downloads (category `network`) always ask
unless explicitly allowed. Only `direction` and `production` agents, or humans, may decide on feedback. See
[Permissions and approvals](../agents/permissions.md).

### Team templates

`studio_team_template` spawns a whole team; agents that already exist by id are updated.

| Template | Members |
|---|---|
| `starter_crew` | Five generalists: director, level designer, gameplay programmer, lighting artist, writer. A new project starts with it. |
| `indie_trio` | Three people who do everything: a director-designer, a programmer and an artist. |
| `aaa_strike_team` | 22: direction, production, five designers, four programmers, five artists, audio, writing, QA, two playtester personas and a critic. |
| `narrative_team` | Narrative designer, two writers, environmental storyteller, audio, critic. |
| `qa_squad` | A QA lead, three playtester personas (newcomer, speedrunner, explorer), a critic and a fixer. |
| `art_team` | Art director with environment, lighting, VFX and technical artists, plus a visual critic. |
| `audio_team` | Sound designer, composer and a gameplay programmer to wire audio to events. |

### Board, feedback and decisions

**Tasks** have a title, description, acceptance criteria, discipline, assignee, priority, dependencies (moving to
`doing` or `done` checks them), links (feedback, entities, assets, captures, playtests) and a comment thread. Their status
moves `backlog → todo → doing → review → done`, or `dropped`. `studio_task_claim` without a task picks the caller's next
task.

**Feedback** has a category (`fun`, `difficulty`, `clarity`, `visuals`, `audio`, `performance`, `bug`, `narrative`,
`accessibility`), a severity, a summary, details, a target, evidence (`playtest`, `captures`, `metrics`, `positions`,
`repro`, `trace`) and a status: `open → accepted → in_progress → fixed → verified`, or `dropped`, `deferred`,
`regressed`. A `fingerprint` deduplicates recurring findings: a repeat bumps the item's occurrence count.

**Decisions** (`studio_decide`) are `act` (creates tasks linked to the feedback), `drop`, `defer` or `merge_into` a
duplicate, always with a rationale everyone sees. The latest playtest metrics are stored as the baseline
(`metrics_before`), optionally with explicit `targets` such as `{"deaths": {"max": 1}}`.

**Measured effect.** When the last task of an accepted item is done, the item becomes `fixed`. The next verification
playtest (a loop stage with `verify_fixed`, or `playtest_compare` with `feedback`) compares before and after:

| Outcome | Result |
|---|---|
| `improved` | The item becomes `verified`. |
| `regressed` | The item becomes `regressed` and its tasks reopen. |
| `unchanged`, `mixed` | The item stays `fixed` for a human to judge. |
| `unmeasured` | Categories without metrics (visuals, audio, narrative) wait until a critic or human verifies them with `studio_feedback_update`. |

Metrics are direction-aware (fewer deaths and higher completion are better) with a 5% tolerance and a noise floor for
timings. Each category is measured by:

| Category | Metrics |
|---|---|
| difficulty | `deaths`, `completion_rate`, `time_to_goal`, `fails`, `damage` |
| clarity | `stuck_seconds`, `completion_rate`, `time_to_goal` |
| performance | `est_fps`, `avg_tick_ms`, `p95_tick_ms` |
| bug | `script_errors`, `fails`, `stuck_seconds` |
| fun | `completion_rate`, `quit_rate`, `coverage` |

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/studio-board.webp · The Studio dock, Board view: Kanban columns backlog, todo, doing, review, done with task cards, priorities, linked feedback and assignee avatars</div>

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/studio-feedback.webp · The Studio dock, Feedback view: an item with the director's verdict, rationale and measured effect as metric deltas</div>

## Loops

A loop is data: stages, stop conditions and runtime state, stored in `studio/loops/<name>.loop.json`.

```json
{
  "name": "playtest_fix_verify",
  "goal": "Make the level fun and fair to complete.",
  "stages": [
    {"id": "playtest", "kind": "playtest", "playtest": {"policy": "goal_seeker", "runs": 3, "seconds": 45},
     "gate": {"skip_if": "not_first_iteration"}},
    {"id": "play", "assignees": ["@playtester"], "instruction": "Play with your persona and file feedback. {{inputs}}"},
    {"id": "triage", "assignees": ["@creative_director|@producer"], "gate": {"skip_if": "no_open_feedback"},
     "instruction": "Triage every open item with studio_decide.\n{{open_feedback}}\nMetrics: {{metrics}}"},
    {"id": "fix", "assignees": ["@design", "@engineering", "@art"], "only_with_tasks": true,
     "instruction": "Your tasks:\n{{my_tasks}}"},
    {"id": "verify", "kind": "playtest", "playtest": {"runs": 3, "seconds": 45}, "verify_fixed": true}
  ],
  "stop": {"max_iterations": 3, "metric_targets": {"completion_rate": {"min": 1}},
           "director_signoff": false, "token_budget": 0, "time_budget_minutes": 0}
}
```

| Element | Meaning |
|---|---|
| Stage kinds | `playtest` stages run inside the engine without a model: they play, file findings as feedback (`file_feedback`, deduplicated) and, with `verify_fixed`, measure the effect of fixed items. `agents` stages produce assignments. |
| Assignees | `@role`, `@discipline`, `@all` or an agent id. `"@a|@b"` takes the first alternative that matches anyone. `only_with_tasks` gives work only to agents holding open tasks (unassigned tasks of a matching discipline go to the least busy one). `parallel` (default true) lets the executor run a stage's agents at the same time. |
| Instruction variables | `{{goal}}`, `{{loop}}`, `{{iteration}}`, `{{max_iterations}}`, `{{stage}}`, `{{agent}}`, `{{role}}`, `{{focus}}`, `{{inputs}}` (reports of earlier stages this iteration, or of `inputs: [stage ids]`), `{{previous}}`, `{{open_feedback}}`, `{{fixed_feedback}}`, `{{my_tasks}}`, `{{todo_tasks}}`, `{{metrics}}`, `{{trends}}`, `{{roster}}` |
| Gates | `skip_if`: `no_open_feedback`, `no_todo_tasks`, `no_fixed_feedback`, `first_iteration`, `not_first_iteration` (one or a list). `approval: "human"` pauses the loop until `studio_loop_advance` with `approve: true`. |
| Success conditions | `metric_targets` (on the iteration's last playtest) and `director_signoff` (a direction or production agent reports `SIGNOFF` or passes `signoff: true`). All must hold. |
| Limits | `max_iterations`, `token_budget` (all studio tokens since the run started), `time_budget_minutes`. Any limit ends the run. |

Loop templates: `playtest_fix_verify`, `art_pass_with_critic`, `balance_tuning`, `vertical_slice_sprint`, `bug_bash`.
Start from one and override the goal or stop conditions, or write the stages yourself.

`studio_loop_status` reports the state: status (`idle`, `running`, `awaiting_approval`, `done`, `stopped`), iteration,
stage, pending assignments with their full prompts, every iteration's stage records (reports, playtest ids and
metrics, filed and verified feedback), metric trends, tokens used and a summary of earlier runs.

**Who executes agent stages?** Whoever drives the loop. The loop is a state machine behind `studio_loop_start` and
`studio_loop_advance`: the engine runs playtest stages itself and hands out assignments for agent stages. The headless
runner, the editor's crew and external agents all drive it through these two tools.

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/studio-loops.webp · The Studio dock, Loops view: an iterations by stages timeline with live progress, playtest results, filed and verified feedback, a metric sparkline and Run, Stop and Approve buttons</div>

## Playtests that play

`playtest_run` copies the current scene into a sandbox engine (the scene you edit, its undo history and its selection
are never touched), presses play and drives the game for a number of seconds with a bot, through the same `sim_input`
tool agents use.

| Policy | Behavior |
|---|---|
| `goal_seeker` | Walks to the nearest unreached entity tagged `goal` or `objective`, steers around `hazard`, `enemy` and `danger`, slides along walls (ray casts), jumps over low obstacles, detours when stuck, quits when it makes no progress for `patience` seconds. Explores if the level has no goals. |
| `explorer` | Covers unexplored 2 m cells of the level, avoiding hazards. |
| `random` | Random headings and jumps. |
| `scripted` | `script: [{t, hold/release/press/click/event...}]`: any `sim_input` arguments at given times. |

The **persona** shapes the bot: `reaction_time` (seconds between decisions), `skill` (steering precision, hazard
clearance, jumping), `curiosity` (detours toward unexplored space) and `patience`. Playtester agents play with their
own persona by default. `runs` aggregates several consecutive seeds.

### Conventions the bots understand

| Your game does | The bot reads it as |
|---|---|
| Tags the player `player` | The entity it drives |
| Tags goals `goal` or `objective` | Where to go |
| Tags dangers `hazard`, `enemy` or `danger` | What to avoid |
| Emits `death`, `died`, `killed` or `respawn` | A death |
| Emits `fail`, `lose` or `game_over` | A fail |
| Emits `damage`, `hurt` or `hit` | Damage |
| Emits `objective`, `goal`, `win`, `level_complete` or `victory` | An objective |
| Emits an event ending in `collected`, or `pickup` | A pickup |
| Emits `checkpoint` | A checkpoint |
| Teleports the player without a death event | A respawn death |

Movement keys default to WASD in world space (`w` = −Z) and `space` to jump; override them with `controls`. A death
event from Wander is one line:

```wander
behavior Lava
  on trigger_enter "player"
    emit "death" with {cause: "lava"}
    let checkpoint = find("Checkpoint")
    if checkpoint then teleport(other, checkpoint) end
  end
end
```

### The report

| Part | Contents |
|---|---|
| Metrics | `completion_rate`, `time_to_goal`, `deaths`, `fails`, `damage`, `objectives`, `stuck_seconds`, `coverage`, `distance`, `quit_rate`, `script_errors`, `avg_tick_ms`, `p95_tick_ms`, `est_fps` |
| Deaths | Causes (the nearest hazard) |
| Runs | Per-run trajectories, events, stuck periods and objectives |
| Screenshots | At notable moments (death, goal, stuck, quit, end) plus every `screenshot_interval` seconds |
| Heatmap | A top-down image: geometry, hazards in red, goals in green, trajectory density, deaths as ×, stuck spots as ○ |
| Findings | The feedback a QA lead would file, with evidence and fingerprints |

Frame cost samples a 960×540 render every 2 s after a warm-up frame. The simulation is deterministic: the same scene
and seed give the same report, timings aside. `playtest_compare` diffs two reports and can record the effect on a
feedback item.

```tool
playtest_run {"policy": "goal_seeker", "runs": 3, "seconds": 45, "persona": {"reaction_time": 0.45, "skill": 0.4, "curiosity": 0.6, "patience": 12}, "label": "newcomer"}
playtest_run {"policy": "scripted", "seconds": 10, "script": [{"t": 0, "hold": ["w"]}, {"t": 2, "press": ["space"]}, {"t": 4, "release": ["w"]}]}
playtest_compare {"before": "P-3", "after": "P-5", "feedback": "F-7"}
```

## How to run a studio

### Quick start from an MCP client

```tool
studio_team_template {"template": "aaa_strike_team"}
studio_loop_define {"template": "playtest_fix_verify", "goal": "Players finish the canyon run without unfair deaths"}
studio_loop_start {"loop": "playtest_fix_verify"}
studio_loop_advance {"loop": "playtest_fix_verify", "reports": [{"agent": "nimbus", "report": "Triaged F-2 and F-3: act on both, see T-4 and T-5."}]}
studio_loop_status {"loop": "playtest_fix_verify"}
```

### Identity: who is calling

Every tool call carries an actor. Studio tools map it to a roster member:

| Actor | Who it is |
|---|---|
| `agent:<id>` | An agent run by the editor's crew or the headless runner |
| `mcp:<client>/<id>` | An MCP client whose `clientInfo.name` ends in `/<agent id>`, for example `skywalker call ... --attach --as claude-code/mira` |
| `as: "<id>"` on studio calls | For clients that cannot rename themselves; each Claude Code subagent shares the session's MCP connection and identifies this way |
| `user`, `mcp:claude-code` (unidentified) | Acts for the human: can decide, sign off and approve |

An identified member cannot act as another member.

### Recipe: run a loop from an external agent

This is how Claude Code, Codex, Gemini CLI or Cursor execute a loop with their own subagents.

1. Start the loop. The result lists `assignments: [{agent, stage, prompt, tasks}]` and whether they may run in
   `parallel`.

    ```tool
    studio_loop_start {"loop": "playtest_fix_verify"}
    ```

2. For each assignment, fetch the agent's brief: its system prompt and the tools it may use.

    ```tool
    studio_agent_brief {"agent": "cirro", "loop_member": true}
    ```

3. Spawn a subagent with that system prompt and the assignment's `prompt`. It calls only the listed tools and passes
   `as` on studio calls:

    ```tool
    studio_task_claim {"as": "cirro"}
    studio_task_update {"task": "T-4", "status": "review", "comment": "Bridge widened to 3 m; deaths 0 in two playtests.", "as": "cirro"}
    ```

4. Report back. Reports may arrive one at a time; the loop moves on when every pending agent has reported (or
   `complete_stage: true`). Include token usage so budgets stay accurate.

    ```tool
    studio_loop_advance {"loop": "playtest_fix_verify", "reports": [{"agent": "cirro", "report": "T-4 done: lava bridge widened.", "usage": {"input_tokens": 41200, "output_tokens": 3100, "model": "claude-opus-5-5"}}]}
    ```

5. Repeat until `status` is `done`, `stopped` or `awaiting_approval`. Answer an approval gate with
   `studio_loop_advance {"loop": "playtest_fix_verify", "approve": true}`.

### The headless runner

```bash
export ANTHROPIC_API_KEY=...                               # keys come from the environment only
skywalker studio run --project MyGame --loop playtest_fix_verify --iterations 3
skywalker studio feedback --project MyGame                  # verdicts and measured effects
skywalker studio status --project MyGame
skywalker studio run --project MyGame --loop bug_bash --dry-run   # print every prompt, no API calls
```

| Option | Effect |
|---|---|
| `--loop NAME` | The loop to run. A template name defines the loop on first use. |
| `--iterations N`, `--goal TEXT` | Override the loop's limit and goal for this run. |
| `--scene FILE` | The scene to load (default `scenes/main.sky.json`). It is saved at the end unless `--no-save` or a dry run. |
| `--dry-run` | A stub model: no API calls, no cost; prints each agent's full prompt. Playtest stages still run. |
| `--mock SCRIPT.json` | A scripted conversation per agent, for tests. |
| `--yes` | Allow `ask` tools. Headless runs have no human, so `ask` tools are declined without it. |
| `--verbose` | Print each agent's prompt and longer excerpts of its replies. |

Other subcommands: `skywalker studio status|agents|board|feedback|loops --project DIR`. Ctrl-C stops the loop cleanly.

**Providers.** Each agent's `provider` picks the API. Keys come from the environment and are never written to disk.

| Provider | Variables |
|---|---|
| `anthropic` | `ANTHROPIC_API_KEY` (and optionally `ANTHROPIC_BASE_URL`) |
| anything else (OpenAI-compatible) | `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_MODEL`; `OPENAI_VISION=0` for text-only models; local servers need no key |

With the Anthropic Messages API the runner uses adaptive thinking with high effort on models that support it, prompt
caching, server-side refusal fallback on supported models, append-only history, and screenshots as image blocks in tool
results. It handles every stop reason: a tool call cut off by `max_tokens` is never run (the model is told to re-issue
it), a refusal discards partial output and runs no tools, and `pause_turn` resumes. It retries 429, 5xx and 529
responses with backoff. With OpenAI-compatible Chat Completions, screenshots follow tool messages as an image user
message, and invalid tool-argument JSON comes back as an `INVALID_JSON` error. Agents get only the tools their
permissions allow; inside loops, loop and roster control tools are withheld. Parallel stages run on worker threads.

The `--mock` script format:

```json
{"agents": {"stratus": [{"text": "Fixing the jump.", "tool_calls": [{"name": "studio_task_claim", "input": {}}], "stop": "end_turn"}]},
 "default": [{"text": "Nothing to do.", "stop": "end_turn"}]}
```

## The editor Studio panel

The **Studio** dock tab has five views:

| View | What it shows |
|---|---|
| **Roster** | Avatar, role, focus, live status and activity, open tasks, tokens and cost. Spawn teams, add agents by role, open the designer. |
| **Board** | A Kanban board with drag-and-drop between columns, priorities, linked feedback, assignee avatars and comments. |
| **Feedback** | Each item with its verdict, rationale and measured effect as metric deltas. Decide from the panel. |
| **Loops** | Iterations by stages with live progress, playtest results, filed and verified feedback, a metric sparkline; run, stop and approve. |
| **Messages** | Channels and threads. |

The crew in the Agents tab is the same roster. Open the **Agent Designer** from the slider button in a chat header,
from a roster card, or in Settings → Crew; edits save to the agent's file (never API keys, which the editor keeps in
the macOS Keychain). Direction and production agents get an extra delegation tool in chat to hand a task to a teammate
and review the report. You see agents at work through presence avatars in the toolbar, a toast over the viewport and the
Activity feed, which records who did what; every edit is undoable.

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/agent-designer.webp · The Agent Designer: role and focus, provider and model, autonomy, per-category permissions, persona, memory and playtester knobs</div>

## Pitfalls

- **Bots steer without a navigation mesh** (ray casts and potential fields); mazes defeat them.
- **Playtests run synchronously** on the engine thread: in the editor, a long playtest blocks the UI while it runs.
- **World-space WASD.** Movement mapping assumes world-space WASD unless `controls` says otherwise; camera-relative
  schemes need a `scripted` policy.
- **Budgets count only known tokens**: the runner, the editor crew, and usage reported with `studio_usage_report` or
  in `studio_loop_advance` reports from external executors.
- **Headless runs have no human.** Without `--yes`, every `ask` tool is declined.

!!! agent "For agents"

    Start with the overview, act as a named member, and keep the board and feedback honest:

    ```tool
    studio_overview {"include_catalog": true}                      # roster, board, feedback, loops, metrics
    studio_inbox {"as": "stratus"}                                  # messages for you
    studio_task_claim {"as": "stratus"}                             # your next task
    playtest_run {"policy": "goal_seeker", "runs": 3, "seconds": 45} # measure before and after a change
    studio_feedback_submit {"category": "difficulty", "severity": "high", "summary": "Lava bridge kills most runs", "evidence": {"playtest": "P-3"}, "fingerprint": "lava-bridge-deaths", "as": "drizzle"}
    studio_decide {"feedback": "F-2", "verdict": "act", "rationale": "Deaths spike at the bridge; widen it.", "tasks": [{"title": "Widen the lava bridge", "acceptance": ["bridge 3 m wide", "deaths at most 1 per run"], "assignee": "@level_designer"}], "targets": {"deaths": {"max": 1}}}
    studio_message_send {"text": "@lighting_artist the lava needs a stronger glow, see F-2", "feedback": "F-2", "as": "nimbus"}
    ```

## Reference

- Tools: [Studio tools](../reference/tools/studio.md), including
  [`studio_overview`](../reference/tools/studio.md#studio_overview),
  [`studio_agent_define`](../reference/tools/studio.md#studio_agent_define),
  [`studio_agent_brief`](../reference/tools/studio.md#studio_agent_brief),
  [`studio_loop_define`](../reference/tools/studio.md#studio_loop_define),
  [`studio_loop_start`](../reference/tools/studio.md#studio_loop_start),
  [`studio_loop_advance`](../reference/tools/studio.md#studio_loop_advance),
  [`playtest_run`](../reference/tools/studio.md#playtest_run),
  [`playtest_compare`](../reference/tools/studio.md#playtest_compare)
- CLI: [`skywalker studio`](../reference/cli.md#studio)
- [Connect an AI agent](../getting-started/connect-agent.md), [The MCP server](../agents/mcp.md)
- Design document: [docs/STUDIO.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/STUDIO.md)
