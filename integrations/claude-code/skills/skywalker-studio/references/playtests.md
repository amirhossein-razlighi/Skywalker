# Playtests

`playtest_run` copies the current scene into a sandbox engine, presses play and drives the game with a bot through the same `sim_input` mechanism you use.
Your scene, undo history and selection are never touched. Deterministic: same scene + seed = same report (timings aside).

## Policies

| Policy | Behavior |
|---|---|
| `goal_seeker` | Walks to the nearest unreached entity tagged `goal`/`objective`, steers around `hazard`/`enemy`/`danger`, slides along walls, jumps low obstacles, detours when stuck, quits when no progress for `patience` seconds. Explores if there are no goals. |
| `explorer` | Covers unexplored 2 m cells of the level, avoiding hazards. Good for coverage and bug hunting. |
| `random` | Random headings and jumps. Good for crashes and softlocks. |
| `scripted` | `script:[{t, hold|release|press|click|event...}]`: any `sim_input` arguments at given seconds. For camera-relative schemes, jump puzzles, specific repros. |

`persona:{reaction_time, skill, curiosity, patience}` shapes the bot (a newcomer: slow, low skill, curious; a speedrunner: fast, high skill, low curiosity).
`runs:N` aggregates N seeds: use at least 3 for any decision.

## Making a game measurable

- Tag the player `player`; goals `goal` or `objective`; dangers `hazard`, `enemy` or `danger`.
- Emit events from Wander: `death`/`died`/`killed`/`respawn`, `fail`/`lose`/`game_over`, `damage`/`hurt`/`hit`, `objective`/`goal`/`win`/`level_complete`/`victory`,
  `*collected`/`pickup`, `checkpoint`. A sudden player teleport without a death event counts as a respawn death.
- Controls default to world-space WASD with `w` = -Z and `space` to jump; pass `controls` for others.

Bots only press inputs; the game must respond. If every run is "stuck" at the spawn point, check the player has a behavior reading `axis("move")` (or WASD keys)
by driving it yourself: `sim_input {axes:[{name:"move", x:0, y:1, ticks:60}]}`, `sim_control {action:"step", ticks:60}`, `sim_trace`.

## Reading a report

Metrics: completion_rate, time_to_goal, deaths, fails, damage, objectives, stuck_seconds, coverage, distance, quit_rate, script_errors, avg_tick_ms,
p95_tick_ms, est_fps. Also: death causes (nearest hazard), per-run trajectories, stuck periods, screenshots at notable moments (death, goal, stuck, quit, end),
a top-down heatmap (hazards red, goals green, trajectory density, deaths x, stuck circles), and `findings` ready for `studio_feedback_submit`.

| Category | Metrics that measure it |
|---|---|
| difficulty | deaths, completion_rate, time_to_goal, fails, damage |
| clarity | stuck_seconds, completion_rate, time_to_goal |
| performance | est_fps, avg/p95 tick ms |
| bug | script_errors, fails, stuck_seconds |
| fun | completion_rate, quit_rate, coverage |

Visuals, audio and narrative have no metrics: attach captures and let a critic or human verify.

## Compare before and after

```text
playtest_run     {policy:"goal_seeker", runs:3, seconds:45, label:"before"}      # P-3
(fix the level)
playtest_run     {policy:"goal_seeker", runs:3, seconds:45, label:"after"}       # P-4
playtest_compare {before:"P-3", after:"P-4", feedback:"F-2"}                     # records the measured effect on the feedback item
```

Metrics are direction-aware (fewer deaths and higher completion are better) with a 5 percent tolerance and a noise floor for timings.

## Limits

- No navigation mesh yet: bots use ray casts and potential fields, so mazes defeat them.
- Playtests run synchronously on the engine thread; long runs block the editor UI while they run.
- Camera-relative movement needs `scripted` or `controls`.
