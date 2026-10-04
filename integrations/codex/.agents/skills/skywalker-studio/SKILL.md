---
name: skywalker-studio
description: "Run a team of AI agents on a Skywalker game - define the roster, use the task board, file feedback, make director decisions with measured effects, run playtest bots, and drive loops (playtest, triage, fix, verify) with subagents acting as roster members. Use for multi-agent game development, QA passes, balance tuning, art passes, \"playtest and fix\", or when you are asked to act as a studio role."
---

# The Studio (multi-agent game development)

The studio lives **in the project** (roster `agents/<id>.agent.json`, board, feedback, decisions, loops, messages, usage under `studio/`),
so the editor's crew, the headless runner (`skywalker studio run`) and external agents like you share one state. Tools: `studio_*` and
`playtest_*`. The studio tools never change the game; the game changes only through the regular engine tools.

Start: `studio_overview {include_catalog:true}` (roster with live status, board counts, feedback awaiting a decision, recent decisions with
their measured effect, loops, latest playtest metrics, usage; the catalog lists roles, disciplines, team and loop templates).

## 1. Identity: who are you?

Every call carries an actor. Studio tools map it to a roster member:

- Unidentified clients (`mcp:claude-code`, `user`) act **for the human**: they may decide on feedback, sign off, approve.
- To act **as a roster member**, pass `as:"<agent id>"` on studio calls (`studio_task_claim`, `studio_feedback_submit`, `studio_message_send`,
  `studio_inbox`, `studio_memory`...). Clients that can rename themselves can instead connect as `<client>/<agent id>`
  (`skywalker call ... --attach --as claude-code/mira`). An identified member cannot impersonate another, and only `direction` and
  `production` agents can `studio_decide`.
- Subagents of Claude Code / Codex / Gemini / Cursor share the session's MCP connection, so they use `as`. Engine edits made by the session
  appear as `mcp:<client>` in history; the roster identity is for coordination.

**Adopt a role properly:** `studio_agent_brief {agent:"<id>", loop_member:true}` returns the member's full system prompt (mission, focus,
persona, standing instructions, memory, the team, etiquette) and the tools it may use (`allow` / `ask`; `ask` means get the human's OK). Follow it.
`loop_member:true` withholds roster/loop control tools, which is right for anyone inside a loop stage.

## 2. Build the team

| Need | Call |
|---|---|
| A ready team | `studio_team_template {template: starter_crew\|indie_trio\|aaa_strike_team\|narrative_team\|qa_squad\|art_team\|audio_team}` |
| One specialist | `studio_agent_define {id, name, role, focus, focus_tags, persona, instructions, autonomy, permissions, reports_to}` (merge semantics: only passed fields change) |
| A playtester persona | `studio_agent_define {id, role:"playtester", playtest:{policy, reaction_time, skill, curiosity, patience}}` |
| Look | `studio_agent_list {include_profiles:true}`, remove with `studio_agent_remove` |

Roles (24): creative_director, art_director, producer, systems_designer, level_designer, narrative_designer, economy_designer, ux_designer,
gameplay_programmer, ai_programmer, graphics_programmer, tools_programmer, environment_artist, lighting_artist, character_artist, vfx_artist,
technical_artist, ui_artist, sound_designer, composer, writer, qa_lead, playtester, critic. Several agents can share a role with different
`focus`. `autonomy`: `observe` (read-only), `ask` (mutations need a human), `autonomous`. `permissions` per tool category: allow / ask / off.

**This plugin's subagents** (creative-director, level-designer, environment-artist, gameplay-programmer, technical-artist, sound-designer,
playtester, critic) use studio ids equal to their role names (`creative_director`, `level_designer`, `environment_artist`, `gameplay_programmer`,
`technical_artist`, `sound_designer`, `playtester`, `critic`). Make sure those roster members exist before invoking them (the
`studio-setup` command does it, or `studio_agent_define {id:"level_designer", role:"level_designer", name:"Mira"}` yourself). If the project already has a roster
(Nimbus, Aurora...), use those ids with `as` and the matching subagent prompt; do not create duplicates.

## 3. The board

`studio_task_create {title, description, acceptance:[...], discipline, assignee, priority, depends_on, feedback:[...], links}`.
Good tasks are small and checkable: a clear title and **acceptance criteria that can be verified** ("bridge is 3 m wide", "deaths <= 1 per
run"). `assignee` is an agent id or `"@role"` (least busy agent with that role). Status flows `backlog > todo > doing > review > done` (or
`dropped`); `studio_task_update {task, status, comment}`; `studio_task_claim {as}` takes your highest-priority todo (or unassigned in your discipline).
Moving to doing/done checks dependencies. Finishing the last task of a feedback item moves it to `fixed`. Read the board with `studio_task_list {status:"open", assignee:"level_designer"}`
(filters: `status` incl. `open`/`all`, `assignee`, `discipline`, `limit`; `mine:true` needs a roster identity, pass `as`) before creating duplicates.

## 4. Feedback, decisions, measured effects

- **File feedback** (playtesters, critics, anyone): `studio_feedback_submit {category: fun|difficulty|clarity|visuals|audio|performance|bug|narrative|accessibility,
  severity, summary, details, target, evidence:{playtest, captures, metrics, positions, repro}, fingerprint}`. Be specific: where (entity/position), what you
  felt or measured, why it matters, repro steps. A stable `fingerprint` dedups repeats (occurrences++ instead of a duplicate).
- **Read the backlog** with `studio_feedback_list {status:"open", category:"bug"}` before filing, so you add evidence to an existing item instead of duplicating it.
- **Decide** (director/producer or the human): `studio_decide {feedback, verdict: act|drop|defer|merge_into, rationale, tasks:[{title, assignee, acceptance}],
  targets:{"deaths":{"max":1}}}`. `act` creates linked tasks and stores the current playtest metrics as a baseline. Always give a rationale: everyone reads it.
  Never decide your own feedback as a non-director; ask the director via `studio_message_send`.
- **Effect**: when all tasks are done the feedback is `fixed`; the next verification playtest (`playtest_compare {before, after, feedback}` or a loop stage
  with `verify_fixed`) records `improved > verified`, `regressed > reopened`, or `unchanged/mixed` (human/critic judges). Visual, audio and narrative items
  have no metrics: a critic verifies them with `studio_feedback_update {status:"verified"}` after re-reviewing with captures.
- Talk: `studio_message_send {text, to, channel, task, feedback}` and `studio_inbox {as}` (read at the start of every task; reading marks read).
  Long-term notes: `studio_memory {action:"note", text}`.

## 5. Playtests that actually play

`playtest_run {policy: goal_seeker|explorer|random|scripted, seconds, runs, seed, persona, agent, label}` plays a **sandbox copy** of the scene with a bot
(your scene and undo history are untouched) and returns metrics (completion_rate, time_to_goal, deaths, fails, damage, stuck_seconds, coverage, est_fps,
script_errors...), death causes, screenshots at notable moments, a top-down heatmap and ready-to-file findings. Same scene + seed = same report.

Make the game **measurable** or the bots are blind: tag the player `player`, goals `goal`/`objective`, dangers `hazard`/`enemy`, and emit events from Wander
(`death`, `fail`, `damage`, `objective`/`win`, `checkpoint`, `pickup`). Bots assume world-space WASD + space; give `controls` otherwise, or use `scripted`.
Use `runs:3` or more for stable numbers. See [references/playtests.md](references/playtests.md).

## 6. Loops (the engine of iteration)

A loop is data: stages + stop conditions. Define: `studio_loop_define {template: playtest_fix_verify|art_pass_with_critic|balance_tuning|vertical_slice_sprint|bug_bash,
goal, stop}` or custom `stages`. Run:

```text
studio_loop_start   {loop:"playtest_fix_verify", goal:"Players finish the canyon run without unfair deaths", max_iterations:3}
   -> engine runs playtest stages itself; returns assignments [{agent, stage, prompt, tasks}], parallel
(for each assignment, run it AS that agent: studio_agent_brief, then the assignment prompt)
studio_loop_advance {loop:"playtest_fix_verify", reports:[{agent:"level_designer", report:"...", usage:{input_tokens:1200, output_tokens:800}}]}   # also signoff:true (director) / approve:true (human gate)
studio_loop_status  {loop:"playtest_fix_verify", history:true}    studio_loop_stop {loop:"playtest_fix_verify"}
```

**Executing assignments from a coding agent:** one subagent per assignment (run `parallel:true` assignments concurrently, e.g. several Task/subagent
calls in one message). Each subagent: `studio_agent_brief` > `studio_inbox` > do the work with engine tools > `studio_task_update` > a short report.
You collect the reports and call `studio_loop_advance` (reports may arrive one at a time). Repeat until `status` is `done`, `stopped` or `awaiting_approval`
(ask the human, then `approve:true`). Details and custom stage design: [references/loops.md](references/loops.md).

Unattended: `skywalker studio run --project DIR --loop NAME --iterations 3` (needs an API key in the environment) runs the same loop with its own models.

## Reporting token usage

Tool calls are counted automatically. Tokens spent with your **own** model (a custom runner, a subagent) are not visible to the engine: report them so budgets and cost tracking are real.

```text
studio_usage_report {agent:"level_designer", model:"claude-sonnet-5-5", input_tokens:12000, output_tokens:3400, cache_read_tokens:80000}
```

`agent` (or `as`) is required and must be on the roster (`studio_agent_list`; otherwise `not_found`). Report real numbers or nothing; loop reports can carry the same `usage` object (see `studio_loop_advance` above).

## Rules of the road

- Stay in your discipline; if something belongs to a teammate, message them or file feedback instead of doing their job.
- Look before you claim: every visual or layout statement needs a `viewport_capture`; every gameplay claim needs a `sim_trace` or a `playtest_run`.
- Measure, then decide. Do not accept "feels better" as verification when a metric exists.
- Small, undoable changes through `batch`; the history shows who did what.
- Do not decide or sign off unless you are the director/producer (or acting for the human). Do not fake `usage`: report real tokens or none.
- Finish every task with a short report: what changed or was found, evidence (ids, capture, playtest id), what is still open.

## Pitfalls

- Task assigned to `@role` that nobody holds: `not_found` error. Create the member or assign an id.
- A playtest with no `player`/`goal` tags or events returns thin metrics: fix the game's instrumentation first. A bot "stuck" at the spawn for the whole run usually means
  the player has no behavior that reacts to input (`axis("move")` / WASD): the bot presses keys, the game ignores them. Verify with `sim_input` + `sim_trace` before blaming the level.
- `studio_decide` as an identified non-director fails; omit `as` to act for the human only when the human is actually deciding.
- Loops stall on `awaiting_approval`: it is a human gate, not an error.
- Do not run `playtest_run` with huge `seconds` x `runs` in the editor: it blocks the UI while it runs.
