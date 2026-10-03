# Loops

A loop file (`studio/loops/<name>.loop.json`) holds definition and runtime state. You define it with `studio_loop_define`.

```json
{
  "name": "playtest_fix_verify",
  "goal": "Make the level fun and fair to complete.",
  "stages": [
    {"id": "playtest", "kind": "playtest", "playtest": {"policy": "goal_seeker", "runs": 3, "seconds": 45},
     "gate": {"skip_if": "not_first_iteration"}},
    {"id": "play", "assignees": ["@playtester"], "instruction": "Play with your persona... {{inputs}}"},
    {"id": "triage", "assignees": ["@creative_director|@producer"], "gate": {"skip_if": "no_open_feedback"},
     "instruction": "Triage every open item with studio_decide...\n{{open_feedback}}\nMetrics: {{metrics}}"},
    {"id": "fix", "assignees": ["@design", "@engineering", "@art"], "only_with_tasks": true,
     "instruction": "Your tasks:\n{{my_tasks}}"},
    {"id": "verify", "kind": "playtest", "playtest": {"runs": 3, "seconds": 45}, "verify_fixed": true}
  ],
  "stop": {"max_iterations": 3, "metric_targets": {"completion_rate": {"min": 1}},
           "director_signoff": false, "token_budget": 0, "time_budget_minutes": 0}
}
```

## Templates

| Template | Use when |
|---|---|
| `playtest_fix_verify` | The core gameplay loop: bots and playtesters play, the director triages, the team fixes, a verification playtest measures the effect (regressions reopen work). |
| `art_pass_with_critic` | Artists polish, a critic reviews captures, the art director decides, artists fix; ends on director sign-off. |
| `balance_tuning` | Several-run playtests; designers tune numbers until difficulty metrics hit targets. |
| `vertical_slice_sprint` | The producer plans tasks, everyone builds, the director reviews and signs off, a playtest verifies. |
| `bug_bash` | Explorer and random bots plus QA hunt bugs, the producer triages, engineers fix, a playtest verifies. |

## Stage kinds

- `playtest`: runs inside the engine without an LLM. `playtest {policy, runs, seconds}`, `file_feedback` (deduplicated), `verify_fixed` (measure the effect of fixed items).
- agent stage (default): `assignees` + `instruction`. Assignee forms: `@role`, `@discipline`, `@all`, an agent id, or `"@a|@b"` (first alternative that matches anyone).
  `only_with_tasks:true` gives work only to agents holding open tasks (unassigned tasks of a matching discipline are assigned to the least busy one).
  `parallel` (default true) lets the executor run the stage's agents at the same time.

## Instruction variables

`{{goal}} {{loop}} {{iteration}} {{max_iterations}} {{stage}} {{agent}} {{role}} {{focus}} {{inputs}}` (reports of earlier stages in this iteration, or of `inputs:[stage ids]`)
`{{previous}} {{open_feedback}} {{fixed_feedback}} {{my_tasks}} {{todo_tasks}} {{metrics}} {{trends}} {{roster}}`.

## Gates and stop conditions

- `gate.skip_if`: `no_open_feedback`, `no_todo_tasks`, `no_fixed_feedback`, `first_iteration`, `not_first_iteration` (one or a list). `gate.approval:"human"` pauses until `studio_loop_advance {approve:true}`.
- `stop`: success conditions (`metric_targets` on the iteration's last playtest, `director_signoff`) must all hold; limits (`max_iterations`, `token_budget`, `time_budget_minutes`) end the run.
  Directors sign off with `signoff:true` or a report that starts with `SIGNOFF`.

## Designing a good loop

1. State the **goal as a measurable outcome** ("completion_rate >= 0.8 and deaths <= 1"), then put the same numbers in `stop.metric_targets`.
2. Put a `playtest` stage first (cheap, objective) and last (`verify_fixed:true`).
3. Keep agent stages narrow: one purpose each (triage, fix, review). Wide stages produce vague reports.
4. Give the director the only `studio_decide` stage; give critics and playtesters report-only stages (they file feedback, they do not fix).
5. Cap `max_iterations` (2-3) and `token_budget` when unattended.
6. For art: `art_pass_with_critic` with captures as the shared evidence (critics must attach `captures` or describe the exact shot: `view`, `eye`, `target`, `samples`).

## Driving a loop from a coding agent (checklist)

1. `studio_loop_status {}` to see existing loops and templates; `studio_loop_define` if needed.
2. `studio_loop_start {loop, goal?, max_iterations?}`.
3. For every assignment: spawn a subagent with the role's prompt (`studio_agent_brief {agent, loop_member:true}`) and the assignment `prompt`; tell it to pass `as:"<agent id>"`.
4. Collect reports. Call `studio_loop_advance {loop, reports:[...]}`. Include real `usage` if you know it.
5. Read the returned status: more assignments, `awaiting_approval` (ask the human), or `done`/`stopped` (summarize: iterations, verified effects, regressions, remaining feedback).
